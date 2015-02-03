#pragma once

#include "websocketbuffer.hh"
#include "websockethandler.hh"

#include <libwebsockets.h>

#include <rapidjson/stringbuffer.h>

#include <chrono>
#include <iostream>
#include <queue>
#include <mutex>

typedef unsigned char byte;

namespace lwsxx
{
  /// Common base class for sessions of both local acceptors and initiators.
  /// Dispatches messages to the relevant handler.
  class WebSocketSession
  {
    friend class WebSockets;
    friend class AcceptorHandler;
    friend class InitiatorHandler;

  public:
    /** Enqueue buffer for sending over the websocket. */
    void send(WebSocketBuffer& buffer)
    {
      // Return if there is no data to actually send
      if (buffer.empty())
        return;

      send(buffer.flush());
    }

  protected:
    WebSocketSession()
      : _handler(nullptr),
        _context(nullptr),
        _wsi(nullptr),
        _rxBufferPos(0),
        _bytesSent(0)
    {}

    WebSocketHandler* _handler;
    libwebsocket_context* _context;
    libwebsocket* _wsi;

  private:
    void send(std::vector<byte> buf);

    int write();

    void receive(byte* data, size_t len, bool isFinalFragment, size_t remainingInPacket);

    /** Called when the initiator is disconnected, or the acceptor is stopped. */
    virtual void onClosed() = 0;

    bool hasDataToWrite() const { return !_txQueue.empty(); }

    std::vector<byte> _rxBuffer;
    size_t _rxBufferPos;

    /// The queue of encoded messages waiting to be sent via the websocket.
    std::queue<std::vector<byte>> _txQueue;
    /// The number of bytes sent in a previous frame for the bytes at the head of the queue.
    size_t _bytesSent;
    std::mutex _txMutex;
  };

  /// Models a remote client connected to our locally hosted acceptor
  class AcceptorSession : public WebSocketSession
  {
    friend class WebSockets;
    friend class WebSocketHandler;
    friend std::ostream& operator<<(std::ostream& stream, const AcceptorSession& session);

  public:
    // Details of the acceptor client this session represents
    const std::string& getHostName() const { return _hostName; }
    const std::string& getIpAddress() const { return _ipAddress; }
    unsigned long getSessionId() const { return _sessionId; }

    void onClosed() override;

  private:
    void initialise(AcceptorHandler* handler, libwebsocket_context* context, libwebsocket* wsi, std::string hostName, std::string ipAddress, unsigned long sessionId);

    // Details of the acceptor client this session represents
    std::string _hostName;
    std::string _ipAddress;
    unsigned long _sessionId;
  };

  inline std::ostream& operator<<(std::ostream& stream, const AcceptorSession& session)
  {
    stream << session.getSessionId() << ' ' << session.getHostName() << '@' << session.getIpAddress();
    return stream;
  }

  /// Models a local client connected to a remotely hosted acceptor
  class InitiatorSession : public WebSocketSession
  {
    friend class WebSockets;
    friend class WebSocketHandler;
    friend std::ostream& operator<<(std::ostream& stream, const InitiatorSession& session);

  public:
    InitiatorSession(
      InitiatorHandler* handler,
      std::string address,
      int port,
      bool sslConnection,
      std::string path,
      std::string host,
      std::string origin,
      std::string protocol);

  private:
    void connect();

    void setContext(libwebsocket_context* context);

    void checkReconnect();

    /** Called when the initiator connects successfully. */
    void onInitiatorEstablished();

    /** Called when the initiator fails to connect. */
    void onInitiatorConnectionError();

    /** Called when the connection is closed. */
    void onClosed() override;

    std::string _address;
    int _port;
    bool _sslConnection;
    std::string _path;
    std::string _host;
    std::string _origin;
    std::string _protocol;
    bool _isConnectRequested;
    bool _isActuallyConnected;
    std::chrono::high_resolution_clock _clock;
    std::chrono::high_resolution_clock::time_point _lastConnectAttempt;
  };

  inline std::ostream& operator<<(std::ostream& stream, const InitiatorSession& session)
  {
    stream
      << session._handler->getName() << ' '
      << "ws://" << session._address << ':' << session._port << session._path
      << " (protocol \"" << session._protocol << "\")";
    return stream;
  }
}
