#include <lwsxx/websocketbuffer.hh>

#include <libwebsockets.h>

using namespace lwsxx;
using namespace std;

WebSocketBuffer::WebSocketBuffer()
  : _bytes(256)
{
  _bytes.resize(LWS_SEND_BUFFER_PRE_PADDING);
}

vector<byte> WebSocketBuffer::flush(bool appendPadding)
{
  // Should not have already been moved
  assert(_bytes.size() != 0);

  if (appendPadding)
    _bytes.resize(_bytes.size() + LWS_SEND_BUFFER_POST_PADDING);

  return move(_bytes);
}

bool WebSocketBuffer::empty() const
{
  return _bytes.size() == LWS_SEND_BUFFER_PRE_PADDING;
}
