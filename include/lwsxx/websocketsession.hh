#pragma once

#include "websockets.hh"
#include "websocketbuffer.hh"

#include <rapidjson/stringbuffer.h>
#include <iostream>
#include <queue>
#include <mutex>

typedef unsigned char byte;

namespace lwsxx
{
  /** Dispatches lws events for both clients and services. For services, there's one instance of this type per connected client. */
  class WebSocketSession final
  {
    friend class WebSockets;
    friend class WebSocketHandler;

  public:
    // Details of the acceptor client this session represents
    const std::string& getHostName() const { return _hostName; }
    const std::string& getIpAddress() const { return _ipAddress; }
    unsigned long getSessionId() const { return _sessionId; }

    /** Enqueue buffer for sending to the client associated with this session. */
    void send(WebSocketBuffer& buffer)
    {
      // Return if there is no data to actually send
      if (buffer.empty())
        return;

      auto bytes = buffer.flush();
      send(move(bytes));
    }

  private:
    WebSocketSession()
      : _context(nullptr),
        _wsi(nullptr),
        _handler(nullptr),
        _rxBufferPos(0),
        _bytesSent(0),
        _hostName(),
        _ipAddress()
    {}

    void initialise(WebSocketHandler* handler, libwebsocket_context* context, libwebsocket* wsi, std::string hostName, std::string ipAddress, unsigned long session);

    void send(std::vector<byte> buf);

    int write();

    void receive(byte* data, size_t len, bool isFinalFragment, size_t remainingInPacket);

    // TODO the following could reasonably be confused as a callback for when clients connect to our server, rather than a callback for when our client connects to another server
    /** Called when the client connects successfully. */
    void onInitiatorConnected();

    /** Called when the client fails to connect. */
    void onInitiatorConnectionError();

    /** Called when the initiator is disconnected, or the acceptor is stopped. */
    void onClosed();

    bool hasDataToWrite() const { return !_txQueue.empty(); }

    libwebsocket_context* _context;
    libwebsocket* _wsi;
    WebSocketHandler* _handler;

    std::vector<byte> _rxBuffer;
    size_t _rxBufferPos;

    /// The queue of encoded messages waiting to be sent via the websocket.
    std::queue<std::vector<byte>> _txQueue;
    /// The number of bytes sent in a previous frame for the bytes at the head of the queue.
    size_t _bytesSent;
    std::mutex _txMutex;

    // Details of the acceptor client this session represents
    std::string _hostName;
    std::string _ipAddress;
    unsigned long _sessionId;
  };

  inline std::ostream& operator<<(std::ostream& stream, const WebSocketSession& session)
  {
    // Only applies to acceptor sessions
    stream << session.getSessionId() << ' ' << session.getHostName() << '@' << session.getIpAddress();
    return stream;
  }
}
