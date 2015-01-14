#pragma once

#include "websockets.hh"
#include "websocketbuffer.hh"

#include <rapidjson/stringbuffer.h>
#include <iostream>
#include <queue>

typedef unsigned char byte;

namespace lwsxx
{
  /** Dispatches lws events for both clients and services. For services, there's one instance of this type per connected client. */
  class WebSocketSession final
  {
    friend class WebSockets;

  public:
    WebSocketSession()
      : _context(nullptr),
        _wsi(nullptr),
        _handler(nullptr),
        _rxBufferPos(0),
        _bytesSent(0)
    {}

    ~WebSocketSession() = default;

    void initialise(WebSocketHandler* handler, libwebsocket_context* context, libwebsocket* wsi);

    void send(std::vector<byte> buf);

    int write();

    void receive(byte* data, size_t len, size_t remaining);

    // TODO the following could reasonably be confused as a callback for when clients connect to our server, rather than a callback for when our client connects to another server
    /** Called when the client connects successfully. */
    void onClientConnected();

    /** Called when the client disconnected. */
    void onClosed();

    /** Called when the client fails to connect. */
    void onClientConnectionError();

    bool hasDataToWrite() const { return !_txQueue.empty(); }

  private:
    libwebsocket_context* _context;
    libwebsocket* _wsi;
    WebSocketHandler* _handler;

    std::vector<byte> _rxBuffer;
    size_t _rxBufferPos;

    /// The queue of encoded messages waiting to be sent via the websocket.
    std::queue<std::vector<byte>> _txQueue;
    /// The number of bytes sent in a previous frame for the bytes at the head of the queue.
    size_t _bytesSent;
  };
}
