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

    void send(WebSocketBuffer& buffer);

    bool hasSession() const { return !_sessions.empty(); }

  protected:
    virtual bool canProcess(std::string protocolName) const { (void)protocolName; return false; };

    virtual void onSessionAdded(WebSocketSession*) {}
    virtual void onSessionRemoved(WebSocketSession*) {}

  private:
    void addSession(WebSocketSession* session);
    void removeSession(WebSocketSession* session);

    std::vector<WebSocketSession*> _sessions;
  };
}
