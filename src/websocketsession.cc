#include <lwsxx/websocketsession.hh>

#include <camshaft/log.hh>

static const unsigned long WEBSOCKET_WRITE_BUFFER_LENGTH = 2048ul;

using namespace lwsxx;
using namespace std;
using namespace std::chrono;

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
  InitiatorHandler* handler,
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
    _protocol(protocol),
    _isConnectRequested(false),
    _isActuallyConnected(false)
{
  assert(handler);

  _handler = handler;
  handler->setSession(this);
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

  if (_isConnectRequested && _isActuallyConnected)
  {
    log::warning("InitiatorSession::connect") << "Already connected";
    return;
  }

  assert(_context != nullptr);
  assert(_wsi == nullptr);

  _isConnectRequested = true;
  _isActuallyConnected = false;
  _lastConnectAttempt = _clock.now();

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
    log::warning("InitiatorSession::connect") << "Initiator connection failed: " << *this;
}

void InitiatorSession::onInitiatorEstablished()
{
  log::info("InitiatorSession::onInitiatorEstablished") << "Initiator established: " << *this;

  _isActuallyConnected = true;
  static_cast<InitiatorHandler*>(_handler)->onInitiatorEstablished();
}

void InitiatorSession::onInitiatorConnectionError()
{
  log::error("InitiatorSession::onInitiatorConnectionError") << "Initiator connection error: " << *this;

  _isActuallyConnected = false;
  _wsi = nullptr;
}

void InitiatorSession::close()
{
  _isActuallyConnected = false;
  _wsi = nullptr;

  log::level("InitiatorSession::close", _isConnectRequested ? LogLevel::Warning : LogLevel::Info);
}

void InitiatorSession::checkReconnect()
{
  if (_isConnectRequested && _isActuallyConnected)
    return;

  auto ms = duration_cast<milliseconds>(_clock.now() - _lastConnectAttempt);

  if (ms > milliseconds(5000))
    connect();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void AcceptorSession::initialise(AcceptorHandler* handler, libwebsocket_context* context, libwebsocket* wsi, std::string hostName, std::string ipAddress, unsigned long sessionId)
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

  log::info("AcceptorSession::initialise") << "Acceptor client initialised: " << *this;

  handler->addSession(this);
}

void AcceptorSession::close()
{
  log::info("AcceptorSession::close") << "Acceptor client closed: " << *this;

  static_cast<AcceptorHandler*>(_handler)->removeSession(this);

  // Destroy ourselves (note we are created using placement new so need to do this)
  this->~AcceptorSession();
}
