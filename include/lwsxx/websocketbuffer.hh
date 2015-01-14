#pragma once

#include <vector>

namespace lwsxx
{
  typedef unsigned char byte;

  class WebSocketBuffer
  {
  public:
    WebSocketBuffer();

    inline void append(byte c) { _bytes.push_back(c); }

    std::vector<byte> flush(bool appendPadding = true);

    bool empty() const;

  private:
    std::vector<byte> _bytes;
  };
}
