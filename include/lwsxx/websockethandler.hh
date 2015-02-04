#pragma once

#include <queue>
#include <string>
#include <vector>

#include <rapidjson/stringbuffer.h>

typedef unsigned char byte;

namespace lwsxx
{
  class WebSocketBuffer;
  class WebSocketSession;
  class InitiatorSession;

  class WebSocketHandler
  {
    friend class WebSocketSession;
    friend class AcceptorSession;
    friend class InitiatorSession;
    friend class WebSockets;

  public:
    /** Send the specified buffer to all connected clients. */
    virtual void send(WebSocketBuffer& buffer) = 0;

    virtual std::string getName() const = 0;

  protected:
    /** Specifies whether this handler should be associated to an inbound connection. Applies to services only, not clients. */
    virtual bool canProcess(std::string protocolName) const { (void)protocolName; return false; };

    /** Called when a complete message has been received for processing. */
    virtual void receiveMessage(WebSocketSession* session, std::vector<byte>& message) = 0;

    /** Inspect/modify the queue before sending, and optionally veto the send. */
    virtual bool onBeforeSend(WebSocketSession* session, std::queue<std::vector<byte>>& txQueue) { return true; }
  };

  class InitiatorHandler : public WebSocketHandler
  {
    friend class InitiatorSession;

  public:
    void send(WebSocketBuffer& buffer) override;

    bool isConnected() const;

  protected:
    InitiatorHandler()
      : _session(nullptr)
    {}

    virtual void onInitiatorEstablished() {};

  private:
    void setSession(InitiatorSession* session);

    InitiatorSession* _session;
  };

  class AcceptorHandler : public WebSocketHandler
  {
    friend class AcceptorSession;

  public:
    void send(WebSocketBuffer& buffer) override;

    size_t getSessionCount() const { return _sessions.size(); }

  protected:
    virtual void onSessionAdded(WebSocketSession*) {}
    virtual void onSessionRemoved(WebSocketSession*) {}

  private:
    void addSession(WebSocketSession* session);
    void removeSession(WebSocketSession* session);

    std::vector<WebSocketSession*> _sessions;
  };
}
