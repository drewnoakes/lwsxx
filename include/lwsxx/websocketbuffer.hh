#pragma once

#include <vector>

namespace lwsxx
{
  typedef unsigned char byte;

  class WebSocketBuffer
  {
  public:
    WebSocketBuffer();

    inline void append(byte b) { _bytes.push_back(b); }

    inline void append(const byte* bytes, std::size_t len) { _bytes.insert(_bytes.end(), bytes, bytes + len); }

    std::vector<byte> flush(bool appendPadding = true);

    bool empty() const;

  private:
    std::vector<byte> _bytes;
  };
}
