#include <lwsxx/websockets.hh>

#include <camshaft/log.hh>
#include <camshaft/memory.hh>

// TODO break lwsxx dependency upon camshaft/rapidjson
#include <camshaft/lwsxx/websocketjsonbuffer.hh>
#include <rapidjson/writer.h>

using namespace lwsxx;
using namespace std;

typedef unsigned char byte;

static unsigned long acceptorSessionId = 0;

WebSockets::WebSockets(int port)
  : _port(port),
    _context(nullptr)
{
  // LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_DEBUG |
  // LLL_PARSER | LLL_HEADER | LLL_EXT | LLL_CLIENT | LLL_LATENCY

  // Adapt log level based upon felix's config
  // NOTE this assumes that log::minLevel doesn't change over time
  int levelMask = 0;
  if (log::minLevel <= LogLevel::Error)
    levelMask |= LLL_ERR;
  if (log::minLevel <= LogLevel::Warning)
    levelMask |= LLL_WARN;
  if (log::minLevel <= LogLevel::Info)
    levelMask |= LLL_NOTICE;
//  if (log::minLevel <= LogLevel::Trace)
//    levelMask |= LLL_INFO;

  lws_set_log_level(
    levelMask,
    [](int level, const char* msg)
    {
      // Trim the newline character
      int len = (int)strlen(msg);
      string l(msg, 0, (ulong)max(0, len - 1));

      if (level == LLL_ERR)
        log::error("libwebsockets") << l;
      else if (level == LLL_WARN)
        log::warning("libwebsockets") << l;
      else if (level == LLL_NOTICE)
        log::info("libwebsockets") << l;
      else
        log::verbose("libwebsockets") << l;
    });
}

WebSockets::~WebSockets()
{
  if (_context)
    libwebsocket_context_destroy(_context);
}

void WebSockets::addAcceptor(WebSocketHandler* acceptorHandler, string protocol)
{
  if (_context != nullptr)
    throw runtime_error("Already started");

  assert(std::find_if(_acceptorDetails.begin(), _acceptorDetails.end(),
    [&](AcceptorDetails& s) { return s.handler == acceptorHandler; }) == _acceptorDetails.end());

  _acceptorDetails.push_back({acceptorHandler, protocol});
}

void WebSockets::addInitiator(
  WebSocketHandler* initiatorHandler,
  std::string address,
  int port,
  bool sslConnection,
  std::string path,
  std::string host,
  std::string origin,
  std::string protocol)
{
  if (_context != nullptr)
    throw runtime_error("Already started");

  auto initiator = make_unique<InitiatorSession>(initiatorHandler, address, port, sslConnection, path, host, origin, protocol);
  _initiatorSessions.push_back(move(initiator));
}

void WebSockets::start()
{
  if (_context != nullptr)
    throw runtime_error("Already started");

  // LWS requires there to always be at least one protocol
  // HTTP always goes through the first in the array
  _protocols.push_back({
    "",                            // name
    callback,                      // callback
    max({sizeof(AcceptorSession),
         sizeof(shared_ptr<HttpRequest>)}),
                                   // per session data size (applies to accepted connections only)
    4096,                          // rx buffer size
    0,                             // protocol id
    nullptr,                       // per-protocol user data
    nullptr, 0                     // unused
  });

  // Add the initiator protocols
  for (auto const& initiator : _initiatorSessions)
  {
    _protocols.push_back({
      initiator->_protocol.c_str(), // protocol name
      callback,                    // callback
      0,                           // per session data size (doesn't apply to initiators)
      4096,                        // rx buffer size
      0,                           // protocol id
      nullptr,                     // per-protocol user data
      nullptr, 0                   // unused
    });
  }

  // Add the acceptor protocols
  for (auto const& acceptor : _acceptorDetails)
  {
    _protocols.push_back({
      acceptor.protocol.c_str(),   // protocol name
      callback,                    // callback
      sizeof(AcceptorSession),     // per session data size
      4096,                        // rx buffer size
      0,                           // protocol id
      nullptr,                     // per-protocol user data
      nullptr, 0                   // unused
    });
  }

  // Push the sentinel
  _protocols.push_back({ nullptr, nullptr, 0, 0, 0, nullptr, nullptr, 0 });

  // Create the LWS context
  lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.protocols = _protocols.data();
  info.port = _port;
  info.gid = -1;
  info.uid = -1;
  info.user = this;

  _context = libwebsocket_create_context(&info);

  if (_context == nullptr)
    throw runtime_error("Error creating libwebsockets context");

  for (auto& initiator : _initiatorSessions)
  {
    initiator->setContext(_context);
    initiator->connect();
  }
}

void WebSockets::service(unsigned int timeoutMillis)
{
  assert(_context != nullptr);

  for (auto& initiator : _initiatorSessions)
    initiator->checkReconnect();

  int res = libwebsocket_service(_context, (int)timeoutMillis);

  if (res < 0)
  {
    log::error("WebSockets::service") << "libwebsocket_service returned error code: " << res;
//    throw runtime_error("libwebsocket_service returned an error code"); // TODO provide details of error
  }
}

string getHeader(libwebsocket* wsi, lws_token_indexes h)
{
  int len = lws_hdr_total_length(wsi, h) + 1;

  const int MaxHeaderLength = 4096;

  if (len == 1 || len >= MaxHeaderLength)
    return "";

  char buf[MaxHeaderLength];
  int bytesCopied = lws_hdr_copy(wsi, buf, len, h);

  assert(bytesCopied + 1 == len);
  assert(buf[len - 1] == '\0');

  return string(buf, (size_t)(len - 1));
}

int getHeaderInt(libwebsocket* wsi, lws_token_indexes h, int defaultValue)
{
  string str = getHeader(wsi, h);

  if (str.empty())
    return defaultValue;

  try
  {
    return stoi(str);
  }
  catch (std::exception&)
  {
    log::error("getHeaderInt") << "Error parsing header string as integer: " << str;
    return defaultValue;
  }
}

int WebSockets::callback(
  libwebsocket_context* context,
  libwebsocket* wsi,
  libwebsocket_callback_reasons reason,
  void* user,
  void* in,
  size_t len)
{
  if (user == nullptr)
    return 0;

  auto webSockets = static_cast<WebSockets*>(libwebsocket_context_user(context));

  auto session = static_cast<WebSocketSession*>(user);
  auto request = static_cast<shared_ptr<HttpRequest>*>(user);

  switch (reason)
  {
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////// HTTP ONLY
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    case LWS_CALLBACK_FILTER_HTTP_CONNECTION:
    {
      bool isGet = lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI) != 0;
      bool isPost = lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI) != 0;

      if (!(isGet ^ isPost))
      {
        // TODO return a 405 Method Not Allowed -- libwebsockets_return_http_status ??
        // Initialise the user data object anyway, as we will still receive LWS_CALLBACK_CLOSED_HTTP
        new (request) shared_ptr<HttpRequest>();
        return 1;
      }

      size_t contentLength = static_cast<size_t>(getHeaderInt(wsi, WSI_TOKEN_HTTP_CONTENT_LENGTH, 0));

      assert(in);
      assert(len > 0);

      HttpMethod method = isGet ? HttpMethod::GET : HttpMethod::POST;
      string url(static_cast<const char*>(in), len);

      string queryString = getHeader(wsi, WSI_TOKEN_HTTP_URI_ARGS);

      log::info("WebSockets::callback")
        << "Processing HTTP " << (method == HttpMethod::GET ? "GET" : "POST")
        << " request for URL: " << url
        << (queryString.empty() ? "" : "?" + queryString);

      HttpRouteDetails* matchingHandler = nullptr;

      for (auto& handler : webSockets->_httpRoutes)
      {
        if (method == handler.method && regex_match(url, handler.urlPattern))
        {
          matchingHandler = &handler;
          break;
        }
      }

      if (matchingHandler == nullptr)
      {
        log::warning("WebSockets::callback") << "No handler for HTTP " << (method == HttpMethod::GET ? "GET" : "POST") << " request for URL: " << url;
        // TODO return 404 Not Found -- libwebsockets_return_http_status ??
        // Initialise the user data object anyway, as we will still receive LWS_CALLBACK_CLOSED_HTTP
        new (request) shared_ptr<HttpRequest>();
        return 1;
      }

      auto req = new HttpRequest(context, wsi, contentLength, url, queryString, method, matchingHandler->callback);
      new (request) shared_ptr<HttpRequest>(req);

      break;
    }
    case LWS_CALLBACK_HTTP:
    {
      // 'in' here contains the URL
      if ((*request)->contentLength() == 0)
      {
        // No body expected, so invoke callback immediately
        (*request)->invokeCallback(*request);
      }
      break;
    }
    case LWS_CALLBACK_HTTP_BODY:
    {
      // 'in' here contains body data (possibly chunked)
      assert((*request)->contentLength() != 0);
      assert(len != 0);
      assert(in != nullptr);
      (*request)->appendBodyChunk(static_cast<byte*>(in), len);
      break;
    }
    case LWS_CALLBACK_HTTP_BODY_COMPLETION:
    {
      assert((*request)->contentLength() != 0);
      (*request)->invokeCallback(*request);
      break;
    }
    case LWS_CALLBACK_HTTP_WRITEABLE:
    {
      if (!(*request)->_headersSent)
      {
        unsigned char buffer[4096];
        unsigned char* p = buffer + LWS_SEND_BUFFER_PRE_PADDING;
        unsigned char* end = p + sizeof(buffer) - LWS_SEND_BUFFER_PRE_PADDING;

        if (lws_add_http_header_status(context, wsi, (unsigned)(*request)->_responseCode, &p, end))
          return 1;

        if (lws_add_http_header_by_token(context, wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
          reinterpret_cast<const unsigned char*>((*request)->_responseContentType.data()),
          static_cast<int>((*request)->_responseContentType.size()),
          &p, end))
          return 1;

        // Add CORS headers

        //     "Access-Control-Allow-Origin" "*"
        if (lws_add_http_header_by_name(context, wsi,
          reinterpret_cast<const unsigned char*>("Access-Control-Allow-Origin:"),
          reinterpret_cast<const unsigned char*>("*"),
          sizeof("*") - 1, &p, end))
          return 1;

        //     "Access-Control-Allow-Methods" "POST,GET,OPTIONS,PUT,DELETE"
        if (lws_add_http_header_by_name(context, wsi,
          reinterpret_cast<const unsigned char*>("Access-Control-Allow-Methods:"),
          reinterpret_cast<const unsigned char*>("POST,GET,OPTIONS,PUT,DELETE"),
          sizeof("POST,GET,OPTIONS,PUT,DELETE") - 1, &p, end))
          return 1;

        //     "Access-Control-Allow-Headers"  "*"
        if (lws_add_http_header_by_name(context, wsi,
          reinterpret_cast<const unsigned char*>("Access-Control-Allow-Headers:"),
          reinterpret_cast<const unsigned char*>("*"),
          sizeof("*") - 1, &p, end))
          return 1;

        if (lws_add_http_header_content_length(context, wsi,
          (*request)->_responseBody.size() - LWS_SEND_BUFFER_PRE_PADDING,
          &p, end))
          return 1;

        if (lws_finalize_http_header(context, wsi, &p, end))
          return 1;

        int n = libwebsocket_write(wsi,
          buffer + LWS_SEND_BUFFER_PRE_PADDING,
          (size_t)(p - (buffer + LWS_SEND_BUFFER_PRE_PADDING)),
          LWS_WRITE_HTTP_HEADERS);

        if (n < 0)
          return 1;

        (*request)->_headersSent = true;

        libwebsocket_callback_on_writable(context, wsi);
        return 0;
      }

      unsigned long remaining = (*request)->_responseBody.size() - (*request)->_responseBodyPos;

      while (remaining != 0 && !lws_send_pipe_choked(wsi) && !lws_partial_buffered(wsi))
      {
        size_t n = std::min(4096ul, remaining);

        int m = libwebsocket_write(wsi,
          (*request)->_responseBody.data() + (*request)->_responseBodyPos,
          n, LWS_WRITE_HTTP);

        if (m < 0)
          return 1;

        (*request)->_responseBodyPos += (size_t)m;

        // While still active, extend timeout
        if (m != 0)
          libwebsocket_set_timeout(wsi, PENDING_TIMEOUT_HTTP_CONTENT, 5);

        remaining = (*request)->_responseBody.size() - (*request)->_responseBodyPos;
      }

      if (remaining != 0)
        libwebsocket_callback_on_writable(context, wsi);

      assert(!lws_partial_buffered(wsi));

      return 1;
    }
    case LWS_CALLBACK_CLOSED_HTTP:
    {
      if ((*request).get() != nullptr)
        (*request)->abort();
      request->~shared_ptr<HttpRequest>();
      return 0;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////// ACCEPTORS ONLY
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
    {
      // A new client has connected to our service
      new (session) AcceptorSession(); // TODO if we return non-zero, will this still be closed?

      string protocolName = in ? string(static_cast<char*>(in)) : "";

      bool found = false;
      for (auto& acceptor : webSockets->_acceptorDetails)
      {
        if (acceptor.handler->canProcess(protocolName))
        {
          char hostName[256];
          char ipAddress[32];

          int fd = libwebsocket_get_socket_fd(wsi);
          libwebsockets_get_peer_addresses(context, wsi, fd, hostName, sizeof(hostName), ipAddress, sizeof(ipAddress));

          static_cast<AcceptorSession*>(session)->initialise(acceptor.handler, context, wsi, hostName, ipAddress, acceptorSessionId++);
          found = true;
          break;
        }
      }

      if (!found)
      {
        log::error("WebSockets::callback") << "No acceptor handler claimed incoming connection—closing";
        return -1;
      }

      break;
    }
    case LWS_CALLBACK_ESTABLISHED:
    {
      log::info("WebSockets::callback") << "LWS callback established for acceptor client: " << *static_cast<AcceptorSession*>(session);
      break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE:
    {
      assert(context == session->_context);
      assert(wsi == session->_wsi);
      return session->write();
    }
    case LWS_CALLBACK_RECEIVE:
    {
      assert(context == session->_context);
      assert(wsi == session->_wsi);
      if (len != 0)
      {
        session->receive(
          static_cast<byte*>(in),
          len,
          libwebsocket_is_final_fragment(wsi) != 0,
          libwebsockets_remaining_packet_payload(wsi));
      }
      else
      {
        log::warning("WebSockets::callback") << "Received a zero-length message";
      }
      break;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////// COMMON TO ACCEPTORS AND INITIATORS
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    case LWS_CALLBACK_CLOSED:
    {
      assert(context == session->_context);
      assert(wsi == session->_wsi);
      session->_handler->removeSession(session);
      session->onClosed();
      // TODO is this the right thing to do for all sessions (client & server)?
      session->~WebSocketSession();
      break;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////// INITIATORS ONLY
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
    {
      assert(context == session->_context);
      assert(wsi == session->_wsi);
      assert(session->_handler != nullptr);
      static_cast<InitiatorSession*>(session)->onInitiatorEstablished();
      if (session->hasDataToWrite())
        libwebsocket_callback_on_writable(session->_context, session->_wsi);
      break;
    }
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    {
      // Unable to complete handshake with remote server
      assert(context == session->_context);
      assert(wsi == session->_wsi);
      static_cast<InitiatorSession*>(session)->onInitiatorConnectionError();
      break;
    }
    case LWS_CALLBACK_CLIENT_WRITEABLE:
    {
      assert(context == session->_context);
      assert(wsi == session->_wsi);
      return session->write();
    }
    case LWS_CALLBACK_CLIENT_RECEIVE:
    {
      assert(context == session->_context);
      assert(wsi == session->_wsi);
      session->receive(
        static_cast<byte*>(in),
        len,
        libwebsocket_is_final_fragment(wsi) != 0,
        libwebsockets_remaining_packet_payload(wsi));
      break;
    }
    case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
    {
      assert(context == session->_context);
      assert(wsi == session->_wsi);
      if (strcmp(static_cast<const char*>(in), "deflate-stream") == 0 ||
          strcmp(static_cast<const char*>(in), "deflate-frame")  == 0 ||
          strcmp(static_cast<const char*>(in), "x-google-mux")   == 0)
      {
        log::info("WebSockets::callback") << "Denied extension: " << static_cast<const char*>(in);
        return 1;
      }
      break;
    }

    default:
      break;
  }

  return 0;
}

void WebSockets::addHttpRoute(HttpMethod method, regex urlPattern, std::function<void(shared_ptr<HttpRequest>)> callback)
{
  _httpRoutes.emplace_back(method, urlPattern, callback);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

HttpRequest::HttpRequest(libwebsocket_context* context, libwebsocket* wsi, size_t contentLength, string url, string queryString, HttpMethod method, function<void(shared_ptr<HttpRequest>)>& callback)
: _context(context),
  _wsi(wsi),
  _contentLength(contentLength),
  _url(url),
  _queryString(queryString),
  _method(method),
  _callback(callback),
  _bodyData(contentLength),
  _bodyDataPos(0),
  _headersSent(false),
  _responseBody(),
  _responseBodyPos(LWS_SEND_BUFFER_PRE_PADDING),
  _responseCode(HttpStatus::Unknown),
  _responseContentType(),
  _isAborted(false)
{
  assert(context != nullptr);
  assert(wsi != nullptr);
}

void HttpRequest::appendBodyChunk(byte* data, size_t len)
{
  assert(_bodyData.size() == _contentLength);
  _bodyData.resize(_bodyData.size() + len);
  std::copy(data, data + len, _bodyData.begin() + _bodyDataPos);
  _bodyDataPos += len;
}

void HttpRequest::respond(HttpStatus responseCode, std::string contentType, WebSocketBuffer responseBody)
{
  if (_isAborted)
    return;

  // Send headers

  _responseCode = responseCode;
  _responseContentType = contentType;
  _responseBody = move(responseBody.flush(false));

  libwebsocket_callback_on_writable(_context, _wsi);
}

void HttpRequest::invokeCallback(shared_ptr<HttpRequest> request)
{
  assert(_bodyDataPos == _contentLength);

  // This looks a bit weird, but we need to provide the callback the same shared_ptr that
  // lwsxx is holding so that we don't segfault when it destroys the wsi for a closed connection
  // which client code may later attempt to respond to asynchronously.
  assert(request.get() == this);

  try
  {
    _callback(request);
  }
  catch (http_exception& ex)
  {
    camshaft::WebSocketJsonBuffer buffer;
    rapidjson::Writer<camshaft::WebSocketJsonBuffer> writer(buffer);
    writer.StartObject();
    writer.String("error");
    writer.String(ex.what());
    writer.EndObject();

    respond(ex.httpStatus(), "application/json", buffer);
  }
}
