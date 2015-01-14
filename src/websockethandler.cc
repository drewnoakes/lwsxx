#include <lwsxx/websockethandler.hh>

#include <lwsxx/websocketsession.hh>

using namespace lwsxx;
using namespace std;

void WebSocketHandler::addSession(WebSocketSession* session)
{
  _sessions.push_back(session);
  onSessionAdded(session);
}

void WebSocketHandler::removeSession(WebSocketSession* session)
{
  _sessions.erase(std::remove(_sessions.begin(), _sessions.end(), session));
}

void WebSocketHandler::send(WebSocketBuffer& buffer)
{
  // Return if there is no data to actually send
  if (buffer.empty())
    return;

  auto bytes = buffer.flush();

  if (_sessions.size() == 1)
  {
    _sessions[0]->send(move(bytes));
  }
  else
  {
    for (auto session : _sessions)
      session->send(bytes);
  }
}
