#include <lwsxx/websockethandler.hh>

#include <lwsxx/websocketsession.hh>

#include <algorithm>

using namespace lwsxx;
using namespace std;

void AcceptorHandler::addSession(WebSocketSession* session)
{
  _sessions.push_back(session);
  onSessionAdded(session);
}

void AcceptorHandler::removeSession(WebSocketSession* session)
{
  _sessions.erase(std::remove(_sessions.begin(), _sessions.end(), session));
  onSessionRemoved(session);
}

void AcceptorHandler::send(WebSocketBuffer& buffer)
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

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void InitiatorHandler::send(WebSocketBuffer& buffer)
{
  // Return if there is no data to actually send
  if (buffer.empty())
    return;

  auto bytes = buffer.flush();
  _session->send(move(bytes));
}

void InitiatorHandler::setSession(InitiatorSession* session)
{
  assert(session);
  assert(_session == nullptr);

  _session = session;
}

bool InitiatorHandler::isConnected() const
{
  return _session != nullptr && _session->_isActuallyConnected;
}
