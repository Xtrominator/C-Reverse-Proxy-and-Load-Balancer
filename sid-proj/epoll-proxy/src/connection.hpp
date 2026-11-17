// ===========================================================================
// connection.hpp  --  the state of ONE proxied client <-> backend pair
// ===========================================================================
//
//        client                    PROXY                       backend
//   (browser, wrk)      +---------------------------+       (your app server)
//                       |                           |
//    client socket <--> |  c2b buffer  ---------->  | <--> backend socket
//      (fd A)           |  b2c buffer  <----------  |        (fd B)
//                       +---------------------------+
//
//   c2b = "client to backend": bytes read from the client, waiting to be written
//         to the backend.
//   b2c = "backend to client": bytes read from the backend, waiting to be written
//         to the client.
//
// Why buffers? A socket can only accept a limited amount per write() call. If the
// destination is slower than the source, leftover bytes must wait somewhere. We
// also STOP reading from the source when its buffer is full ("backpressure"),
// so one slow client cannot make us use unlimited memory.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "backend.hpp"

// A growable byte queue: append at the back, consume from the front.
class Buffer {
 public:
  size_t size() const { return data_.size() - head_; }
  bool empty() const { return size() == 0; }
  const char* data() const { return data_.data() + head_; }

  void append(const char* p, size_t n) {
    // If we have already consumed a lot from the front, reclaim that space first.
    if (head_ > 0 && head_ >= data_.size() / 2) {
      data_.erase(0, head_);
      head_ = 0;
    }
    data_.append(p, n);
  }

  // Throw away the first n bytes (they have been sent successfully).
  void consume(size_t n) {
    head_ += n;
    if (head_ >= data_.size()) {
      data_.clear();
      head_ = 0;
      if (data_.capacity() > 64 * 1024) std::string().swap(data_);  // give big memory back
    }
  }

 private:
  std::string data_;  // std::string is just a convenient resizable byte array
  size_t head_ = 0;   // index of the first unconsumed byte
};

enum class Side { Client, Backend };

struct Connection;

// A "Handle" is what we give to epoll for each socket. When epoll reports an
// event it gives the Handle pointer back, so we instantly know WHICH connection
// and WHICH side (client/backend) the event is about, without any map lookup.
struct Handle {
  Connection* conn = nullptr;  // nullptr marks the special "listening socket" handle
  Side side = Side::Client;
  int fd = -1;                 // -1 means "closed / not open"
  bool in_epoll = false;       // have we registered this fd with epoll yet?
  uint32_t events = 0;         // which events we currently asked epoll to watch
};

struct Connection {
  enum class State {
    Connecting,   // non-blocking connect() to the backend is still in progress
    Established,  // backend connected; data flows both ways
  };

  Connection() {
    client.conn = this;
    client.side = Side::Client;
    backend.conn = this;
    backend.side = Side::Backend;
  }

  Handle client;   // socket to the client (fd comes from accept)
  Handle backend;  // socket to the backend (fd comes from connect)
  State state = State::Connecting;

  Backend* backend_ptr = nullptr;  // which backend we are using (nullptr if none yet)
  uint64_t tried_mask = 0;         // bit i set = backend i already failed for this client
  int attempts = 0;                // how many backends we have tried

  Buffer c2b;  // client -> backend
  Buffer b2c;  // backend -> client

  bool client_eof = false;       // client closed its sending side (we read 0 bytes)
  bool backend_eof = false;      // backend closed its sending side
  bool backend_shut_wr = false;  // we already told the backend "no more data from client"
  bool closed = false;           // fully torn down, waiting to be deleted

  std::string client_ip;         // text form of the client's IPv4 address (Ketama's key)
  int64_t last_activity_ms = 0;  // for the idle timeout
  int64_t connect_started_ms = 0;  // for the connect timeout
};
