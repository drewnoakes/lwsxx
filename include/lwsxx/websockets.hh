#pragma once

#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <vector>

// TODO break include of lws.h here
#include <libwebsockets.h>

#include "websocketbuffer.hh"

namespace lwsxx
{
  typedef unsigned char byte;

  class WebSocketHandler;

  struct ClientDetails
  {
    WebSocketHandler* handler;
    std::string address;
    int port;
    bool sslConnection;
    std::string path;
    std::string host;
    std::string origin;
    std::string protocol;
  };

  struct ServiceDetails
  {
    WebSocketHandler* handler;
    std::string protocol;
  };

  enum class HttpMethod
  {
    GET,
    POST
  };

  class WebSockets;

  enum class HttpStatus : short
  {
    Unknown = -1,

    Success = 200,

    BadRequest = 400,
    Forbidden = 403,
    NotFound = 404,

    InternalError = 500
  };

  class http_exception : public std::exception
  {
  public:
    http_exception(HttpStatus status, std::string what)
      : _status(status),
        _what(what)
    {}

    HttpStatus httpStatus() const noexcept { return _status; }

    const char* what() const noexcept override { return _what.c_str(); }

  private:
    HttpStatus _status;
    std::string _what;
  };

  class not_found_exception : public http_exception
  {
  public:
    not_found_exception(std::string what)
      : http_exception(HttpStatus::NotFound, what)
    {}
  };

  class bad_request_exception : public http_exception
  {
  public:
    bad_request_exception(std::string what)
      : http_exception(HttpStatus::BadRequest, what)
    {}
  };

  class HttpRequest
  {
  public:
    HttpRequest(libwebsocket_context* context, libwebsocket* wsi, size_t contentLength, std::string url, std::string queryString, HttpMethod method, std::function<void(std::shared_ptr<HttpRequest>)>& callback);

    std::string url() const { return _url; }
    std::string queryString() const { return _queryString; }
    size_t contentLength() const { return _contentLength; }
    HttpMethod method() const { return _method; }
    std::vector<byte>& bodyData() { return _bodyData; }

    void respond(HttpStatus responseCode, std::string contentType, WebSocketBuffer responseBody);

  private:
    friend class WebSockets;

    /// Indicates that the request has been aborted and writing of a response must not occur
    void abort() { _isAborted = true; }

    void invokeCallback(std::shared_ptr<HttpRequest> request);
    void appendBodyChunk(byte* data, size_t len);

    libwebsocket_context* _context;
    libwebsocket* _wsi;

    size_t _contentLength;
    std::string _url;
    std::string _queryString;
    HttpMethod _method;
    std::function<void(std::shared_ptr<HttpRequest>)>& _callback;
    std::vector<byte> _bodyData;
    size_t _bodyDataPos;

    bool _headersSent;
    std::vector<byte> _responseBody;
    size_t _responseBodyPos;
    HttpStatus _responseCode;
    std::string _responseContentType;
    bool _isAborted;
  };

  struct HttpRouteDetails
  {
    HttpRouteDetails(HttpMethod method, std::regex urlPattern, std::function<void(std::shared_ptr<HttpRequest>)> callback)
      : method(method), urlPattern(urlPattern), callback(callback)
    {}

    HttpMethod method;
    std::regex urlPattern;
    std::function<void(std::shared_ptr<HttpRequest>)> callback;
  };

  class WebSockets
  {
  public:
    WebSockets(int port);

    ~WebSockets();

    void addService(
      WebSocketHandler* serviceHandler,
      std::string protocol);

    void addClient(
      WebSocketHandler* clientHandler,
      std::string address,
      int port,
      bool sslConnection,
      std::string path,
      std::string host,
      std::string origin,
      std::string protocol);

    void start();

    void service(unsigned int timeoutMillis);

    void addHttpRoute(
      HttpMethod method,
      std::regex urlPattern,
      std::function<void(std::shared_ptr<HttpRequest>)> callback);

  private:
    static int callback(libwebsocket_context*, libwebsocket*, libwebsocket_callback_reasons reason, void*, void*, size_t);

    int _port;
    libwebsocket_context* _context;
    std::vector<libwebsocket_protocols> _protocols;
    std::vector<ServiceDetails> _serviceHandlers;
    std::vector<ClientDetails> _clientHandlers;
    std::vector<HttpRouteDetails> _httpRoutes;
  };
}
