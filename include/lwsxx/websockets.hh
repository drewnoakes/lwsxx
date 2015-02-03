#pragma once

#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <vector>

// TODO break include of lws.h here
#include <libwebsockets.h>

#include "http.hh"
#include "websocketbuffer.hh"
#include "websocketsession.hh"

namespace lwsxx
{
  typedef unsigned char byte;

  struct AcceptorDetails
  {
    AcceptorHandler* handler;
    std::string protocol;
  };

  class WebSockets
  {
  public:
    WebSockets(int port);

    ~WebSockets();

    void addAcceptor(
      AcceptorHandler* handler,
      std::string protocol);

    void addInitiator(
      InitiatorHandler* initiatorHandler,
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
    std::vector<AcceptorDetails> _acceptorDetails;
    std::vector<std::unique_ptr<InitiatorSession>> _initiatorSessions;
    std::vector<HttpRouteDetails> _httpRoutes;
  };
}
