#include <lwsxx/websocketsession.hh>

#include <camshaft/log.hh>

static const unsigned long WEBSOCKET_WRITE_BUFFER_LENGTH = 2048ul;

using namespace lwsxx;
using namespace std;

void WebSocketSession::send(vector<byte> buf)
{
  lock_guard<mutex> guard(_txMutex);

  // Give the handler a chance to adjust the queue, or to reject the send
  if (!_handler->onBeforeSend(this, _txQueue))
    return;

  _txQueue.push(move(buf));

  if (_context && _wsi)
    libwebsocket_callback_on_writable(_context, _wsi);
}

int WebSocketSession::write()
{
  lock_guard<mutex> guard(_txMutex);

  assert(_handler);
  assert(_wsi);

  // Fill the outbound pipe with frames of data
  while (!lws_send_pipe_choked(_wsi) && !_txQueue.empty())
  {
    vector<byte>& buffer = _txQueue.front();

    auto totalSize = buffer.size() - LWS_SEND_BUFFER_PRE_PADDING - LWS_SEND_BUFFER_POST_PADDING;

    assert(_bytesSent < totalSize);

    byte* start = buffer.data() + LWS_SEND_BUFFER_PRE_PADDING + _bytesSent;

    unsigned long remainingSize = totalSize - _bytesSent;
    unsigned long frameSize = min(WEBSOCKET_WRITE_BUFFER_LENGTH, remainingSize);

    int writeMode = _bytesSent == 0
      ? LWS_WRITE_TEXT // TODO don't assume the channel contains text
      : LWS_WRITE_CONTINUATION;

    if (frameSize != remainingSize)
      writeMode |= LWS_WRITE_NO_FIN;

    bool storePostPadding = _bytesSent + frameSize < totalSize;
    std::array<byte,LWS_SEND_BUFFER_POST_PADDING> postPadding;
    if (storePostPadding)
      std::copy(start + frameSize, start + frameSize + LWS_SEND_BUFFER_POST_PADDING, postPadding.data());

    int res = libwebsocket_write(_wsi, start, frameSize, (libwebsocket_write_protocol)writeMode);

    if (res < 0)
    {
      log::error("WebSocketSession::write") << "Error " << res << " writing to socket";
      return 1;
    }

    _bytesSent += frameSize;

    if (_bytesSent == totalSize)
    {
      // Done sending this queue item, so ditch it, reset and loop around again
      _txQueue.pop();
      _bytesSent = 0;
    }
    else if (storePostPadding)
    {
      std::copy(postPadding.data(), postPadding.data() + LWS_SEND_BUFFER_POST_PADDING, start + frameSize);
    }

    // Break loop if last write was buffered
    if (lws_partial_buffered(_wsi))
      break;
  }

  // Queue for more writing later on if we still have data remaining
  if (!_txQueue.empty())
    libwebsocket_callback_on_writable(_context, _wsi);

  return 0;
}

void WebSocketSession::receive(byte* data, size_t len, bool isFinalFragment, size_t remainingInPacket)
{
  _rxBuffer.resize(_rxBuffer.size() + len);

  std::copy(data, data + len, _rxBuffer.data() + _rxBufferPos);

  if (remainingInPacket == 0 && isFinalFragment)
  {
    try
    {
      _handler->receiveMessage(this, _rxBuffer);
    }
    catch(exception& ex)
    {
      log::error("WebSocketSession::receive") << "Exception thrown by WebSocketHandler::receiveMessage: " << ex.what();
    }
    _rxBuffer.clear();
    _rxBufferPos = 0;
  }
  else
  {
    _rxBufferPos += len;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

InitiatorSession::InitiatorSession(
  WebSocketHandler* handler,
  string address,
  int port,
  bool sslConnection,
  string path,
  string host,
  string origin,
  string protocol)
  : _address(address),
    _port(port),
    _sslConnection(sslConnection),
    _path(path),
    _host(host),
    _origin(origin),
    _protocol(protocol)
{
  assert(handler);

  _handler = handler;
  _handler->addSession(this);
}

void InitiatorSession::setContext(libwebsocket_context* context)
{
  assert(context != nullptr);
  assert(_context == nullptr);

  _context = context;
}

void InitiatorSession::connect()
{
  log::verbose("InitiatorSession::connect");

  assert(_context != nullptr);
  assert(_wsi == nullptr);

  _wsi = libwebsocket_client_connect_extended(
    _context,
    _address.c_str(),
    _port,
    _sslConnection ? 1 : 0,
    _path.c_str(),
    _host.c_str(),
    _origin.c_str(),
    _protocol.size() ? _protocol.c_str() : nullptr,
    -1, // ietf_version_or_minus_one
    this);

  if (_wsi == nullptr)
    throw runtime_error("WebSocket initiator connect failed");

  log::info("InitiatorSession::connect") << "Initiator connected: " << _address << ':' << _port << _path;
}

void InitiatorSession::onInitiatorEstablished()
{
  log::info("InitiatorSession::onInitiatorEstablished") << "Initiator established: " << _address << ':' << _port << _path;
}

void InitiatorSession::onInitiatorConnectionError()
{
  log::error("InitiatorSession::onInitiatorConnectionError") << "Initiator connection error for " << _handler->getName();

  _wsi = nullptr;
}

void InitiatorSession::onClosed()
{
  // TODO put in logic to automatically reconnect periodically!!
//  this->connect();
  log::warning("InitiatorSession::onClosed");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void AcceptorSession::initialise(WebSocketHandler* handler, libwebsocket_context* context, libwebsocket* wsi, std::string hostName, std::string ipAddress, unsigned long sessionId)
{
  assert(this->_context == nullptr);
  assert(this->_wsi == nullptr);
  assert(this->_handler == nullptr);

  assert(context);
  assert(wsi);
  assert(handler);

  this->_hostName = hostName;
  this->_ipAddress = ipAddress;
  this->_sessionId = sessionId;

  this->_context = context;
  this->_wsi = wsi;
  this->_handler = handler;

  handler->addSession(this);
}

void AcceptorSession::onClosed()
{
  log::warning("AcceptorSession::onClosed");
}
