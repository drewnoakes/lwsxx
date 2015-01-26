#include <lwsxx/websockets.hh>

#include <iostream>
#include <queue>

#include <camshaft/log.hh>
#include <camshaft/memory.hh>

#include <lwsxx/websockethandler.hh>
#include <lwsxx/websocketsession.hh>

// TODO break lwsxx dependency upon camshaft/rapidjson
#include <camshaft/lwsxx/websocketjsonbuffer.hh>
#include <rapidjson/writer.h>

using namespace lwsxx;
using namespace std;

typedef unsigned char byte;

static unsigned long clientSessionId = 0;

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

void WebSockets::addService(WebSocketHandler* serviceHandler, string protocol)
{
  if (_context != nullptr)
    throw runtime_error("Already started");

  assert(std::find_if(_serviceHandlers.begin(), _serviceHandlers.end(), [&](ServiceDetails& s) { return s.handler == serviceHandler; }) == _serviceHandlers.end());
  _serviceHandlers.push_back({serviceHandler, protocol});
}

void WebSockets::addClient(
  WebSocketHandler* clientHandler,
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

  _clientHandlers.push_back({clientHandler, address, port, sslConnection, path, host, origin, protocol});
}

void WebSockets::start()
{
  if (_context != nullptr)
    throw runtime_error("Already started");

  // LWS requires there to always be at least one protocol
  // HTTP always goes through the first in the array
  _protocols.push_back({
    "",                         // name
    callback,                   // callback
    max(sizeof(WebSocketSession),
        sizeof(HttpRequest)),   // per session data size
    4096,                       // rx buffer size
    0,                          // protocol id
    nullptr, 0                  // unused
  });

  // Add the clients' protocols
  for (auto client : _clientHandlers)
  {
    _protocols.push_back({
      client.protocol.c_str(),  // protocol name
      callback,                 // callback
      sizeof(WebSocketSession), // per session data size
      4096,                     // rx buffer size
      0,                        // protocol id
      nullptr, 0                // unused
    });
  }

  // Add the services' protocols
  for (auto service : _serviceHandlers)
  {
    _protocols.push_back({
      service.protocol.c_str(), // protocol name
      callback,                 // callback
      sizeof(WebSocketSession), // per session data size
      4096,                     // rx buffer size
      0,                        // protocol id
      nullptr, 0                // unused
    });
  }

  // Push the sentinel
  _protocols.push_back({ nullptr, nullptr, 0, 0, 0, nullptr, 0 });

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

  for (auto client : _clientHandlers)
  {
    auto session = new WebSocketSession();

    libwebsocket* wsi = libwebsocket_client_connect_extended(
      _context,
      client.address.c_str(),
      client.port,
      client.sslConnection ? 1 : 0,
      client.path.c_str(),
      client.host.c_str(),
      client.origin.c_str(),
      client.protocol.size() ? client.protocol.c_str() : nullptr,
      -1, // ietf_version_or_minus_one
      session);

    if (wsi == nullptr)
    {
      delete session;
      throw runtime_error("WebSocket client connect failed");
    }

    session->initialise(client.handler, _context, wsi, "", "", 0);

    log::info("WebSockets::start") << "Client connected: " << client.address << ':' << client.port << client.path;
  }
}

void WebSockets::service(unsigned int timeoutMillis)
{
  assert(_context != nullptr);

  int res = libwebsocket_service(_context, (int)timeoutMillis);

  if (res < 0)
  {
    log::error("WebSockets::service") << "libwebsocket_service returned error code: " << res;
    throw runtime_error("libwebsocket_service returned an error code"); // TODO provide details of error
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

  WebSockets* webSockets = static_cast<WebSockets*>(libwebsocket_context_user(context));
  WebSocketSession* session = static_cast<WebSocketSession*>(user);
  HttpRequest* request = static_cast<HttpRequest*>(user);

  switch (reason)
  {
    ////// HTTP

    case LWS_CALLBACK_FILTER_HTTP_CONNECTION:
    {
      bool isGet = lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI) != 0;
      bool isPost = lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI) != 0;

      if (!(isGet ^ isPost))
      {
        // TODO return a 405 Method Not Allowed -- libwebsockets_return_http_status ??
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
        return 1;
      }

      new (request) HttpRequest(context, wsi, contentLength, url, queryString, method, matchingHandler->callback);

      break;
    }
    case LWS_CALLBACK_HTTP:
    {
      // 'in' here contains the URL
      if (request->contentLength() == 0)
      {
        // No body expected, so invoke callback immediately
        request->invokeCallback();
      }
      break;
    }
    case LWS_CALLBACK_HTTP_BODY:
    {
      // 'in' here contains body data (possibly chunked)
      assert(request->contentLength() != 0);
      assert(len != 0);
      assert(in != nullptr);
      request->appendBodyChunk(static_cast<byte*>(in), len);
      break;
    }
    case LWS_CALLBACK_HTTP_BODY_COMPLETION:
    {
      assert(request->contentLength() != 0);
      request->invokeCallback();
      break;
    }
    case LWS_CALLBACK_HTTP_WRITEABLE:
    {
      if (!request->_headersSent)
      {
        unsigned char buffer[4096];
        unsigned char* p = buffer + LWS_SEND_BUFFER_PRE_PADDING;
        unsigned char* end = p + sizeof(buffer) - LWS_SEND_BUFFER_PRE_PADDING;

        if (lws_add_http_header_status(context, wsi, (unsigned)request->_responseCode, &p, end))
          return 1;

        if (lws_add_http_header_by_token(context, wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
          reinterpret_cast<const unsigned char*>(request->_responseContentType.data()),
          static_cast<int>(request->_responseContentType.size()),
          &p, end))
          return 1;

        if (lws_add_http_header_content_length(context, wsi,
          request->_responseBody.size() - LWS_SEND_BUFFER_PRE_PADDING,
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

        request->_headersSent = true;

        libwebsocket_callback_on_writable(context, wsi);
        return 0;
      }

      unsigned long remaining = request->_responseBody.size() - request->_responseBodyPos;

      while (remaining != 0 && !lws_send_pipe_choked(wsi) && !lws_partial_buffered(wsi))
      {
        size_t n = std::min(4096ul, remaining);

        int m = libwebsocket_write(wsi,
          request->_responseBody.data() + request->_responseBodyPos,
          n, LWS_WRITE_HTTP);

        if (m < 0)
          return 1;

        request->_responseBodyPos += (size_t)m;

        // While still active, extend timeout
        if (m != 0)
          libwebsocket_set_timeout(wsi, PENDING_TIMEOUT_HTTP_CONTENT, 5);

        remaining = request->_responseBody.size() - request->_responseBodyPos;
      }

      if (remaining != 0)
        libwebsocket_callback_on_writable(context, wsi);

      assert(!lws_partial_buffered(wsi));

      return 1;
    }

    ////// SERVER

    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
    {
      // A new client has connected to our service
      new (session) WebSocketSession();

      string protocolName = in ? string(static_cast<char*>(in)) : "";

      bool found = false;
      for (auto& service : webSockets->_serviceHandlers)
      {
        if (service.handler->canProcess(protocolName))
        {
          char hostName[256];
          char ipAddress[32];

          int fd = libwebsocket_get_socket_fd(wsi);
          libwebsockets_get_peer_addresses(context, wsi, fd, hostName, sizeof(hostName), ipAddress, sizeof(ipAddress));

          session->initialise(service.handler, context, wsi, hostName, ipAddress, clientSessionId++);
          found = true;
          break;
        }
      }

      if (!found)
      {
        log::error("WebSockets::callback") << "No handler claimed client -- closing connection";
        return -1;
      }

      break;
    }
    case LWS_CALLBACK_ESTABLISHED:
    {
      log::info("WebSockets::callback") << "LWS callback established for client: " << *session;
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
      // TODO need to check if final fragment here too, to properly support large messages that span multiple packets
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

    ////// COMMON

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

    ////// CLIENT

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
    {
      assert(context == session->_context);
      assert(wsi == session->_wsi);
      assert(session->_handler != nullptr);
      session->onClientConnected();
      if (session->hasDataToWrite())
        libwebsocket_callback_on_writable(session->_context, session->_wsi);
      break;
    }
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    {
      // Unable to complete handshake with remote server
      assert(context == session->_context);
      assert(wsi == session->_wsi);
      session->onClientConnectionError();
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

void WebSockets::addHttpRoute(HttpMethod method, regex urlPattern, std::function<void(HttpRequest&)> callback)
{
  _httpRoutes.emplace_back(method, urlPattern, callback);
}

HttpRequest::HttpRequest(libwebsocket_context* context, libwebsocket* wsi, size_t contentLength, string url, string queryString, HttpMethod method, function<void(HttpRequest&)>& callback)
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
  _responseContentType()
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
  // Send headers

  _responseCode = responseCode;
  _responseContentType = contentType;
  _responseBody = move(responseBody.flush(false));

  libwebsocket_callback_on_writable(_context, _wsi);
}

void HttpRequest::invokeCallback()
{
  assert(_bodyDataPos == _contentLength);

  try
  {
    _callback(*this);
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
