#pragma once

#include <string>
#include <vector>

#include <rapidjson/stringbuffer.h>

typedef unsigned char byte;

namespace lwsxx
{
  class WebSocketBuffer;
  class WebSocketSession;

  class WebSocketHandler
  {
    friend class WebSocketSession;
    friend class WebSockets;

  public:
    /** Called when a complete message has been received for processing. */
    virtual void receiveMessage(std::vector<byte>& message) = 0;

    /** Send the specified buffer to all connected clients. */
    void send(WebSocketBuffer& buffer);

    /** Send the specified buffer to the specified client. */
    void send(WebSocketBuffer& buffer, WebSocketSession* session);

    bool hasSession() const { return !_sessions.empty(); }

  protected:
    /** Specifies whether this handler should be associated to an inbound connection. */
    virtual bool canProcess(std::string protocolName) const = 0;

    virtual void onSessionAdded(WebSocketSession*) {}
    virtual void onSessionRemoved(WebSocketSession*) {}

  private:
    void addSession(WebSocketSession* session);
    void removeSession(WebSocketSession* session);

    std::vector<WebSocketSession*> _sessions;
  };
}
