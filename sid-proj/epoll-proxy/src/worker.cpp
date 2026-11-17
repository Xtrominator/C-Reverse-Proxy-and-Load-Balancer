#include "worker.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "util.hpp"

namespace {

constexpr int kMaxEvents = 512;             // max events handled per epoll_wait call
constexpr size_t kMaxBuffer = 64 * 1024;    // backpressure limit per direction
constexpr size_t kScratchSize = 16 * 1024;  // size of one recv() call
constexpr int kAcceptBatch = 64;            // accept at most this many per event (fairness)

// Sent to a client when no backend is usable. (Harmless to non-HTTP clients.)
const char kNoBackend[] =
    "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
    "Content-Length: 20\r\nConnection: close\r\n\r\nNo backend available";

}  // namespace

Worker::Worker(int id, const Config& cfg, Balancer& balancer)
    : id_(id), cfg_(cfg), balancer_(balancer), scratch_(kScratchSize) {}

Worker::~Worker() {
  // Free anything still alive (only matters at shutdown).
  for (Connection* c : conns_) {
    if (c->client.fd >= 0) ::close(c->client.fd);
    if (c->backend.fd >= 0) ::close(c->backend.fd);
    delete c;
  }
  for (Connection* c : graveyard_) delete c;
  if (listen_fd_ >= 0) ::close(listen_fd_);
  if (epfd_ >= 0) ::close(epfd_);
}

// ---------------------------------------------------------------------------
// init: create the epoll instance and this worker's listening socket
// ---------------------------------------------------------------------------
bool Worker::init() {
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd_ < 0) {
    LOG_ERROR("epoll_create1: %s", std::strerror(errno));
    return false;
  }

  // SOCK_NONBLOCK : accept() will never block
  // SOCK_CLOEXEC  : fd is not leaked into child processes (good hygiene)
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    LOG_ERROR("socket: %s", std::strerror(errno));
    return false;
  }

  int one = 1;
  // SO_REUSEADDR: allow restarting the proxy right away (ignore TIME_WAIT leftovers).
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  // SO_REUSEPORT: allow SEVERAL sockets (one per worker) to bind the same port;
  // the kernel load-balances incoming connections between them.
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);  // all network interfaces
  addr.sin_port = htons(cfg_.port);           // htons: host byte order -> network byte order
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    LOG_ERROR("bind port %d: %s", cfg_.port, std::strerror(errno));
    return false;
  }
  if (::listen(listen_fd_, cfg_.backlog) < 0) {
    LOG_ERROR("listen: %s", std::strerror(errno));
    return false;
  }

  // Register the listening socket: "tell me when a client is waiting to be accepted".
  listen_handle_.conn = nullptr;
  listen_handle_.fd = listen_fd_;
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.ptr = &listen_handle_;  // epoll hands this pointer back to us with the event
  if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
    LOG_ERROR("epoll_ctl(listen): %s", std::strerror(errno));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// run: THE EVENT LOOP
// ---------------------------------------------------------------------------
void Worker::run() {
  std::vector<epoll_event> events(kMaxEvents);
  last_sweep_ms_ = util::now_ms();

  while (!g_stop.load(std::memory_order_relaxed)) {
    // Sleep until something happens, but wake at least every 200 ms so that we
    // notice g_stop and can run the timeout sweep even when traffic is silent.
    int n = ::epoll_wait(epfd_, events.data(), kMaxEvents, 200);
    now_ms_ = util::now_ms();  // read the clock once per loop, not once per event

    if (n < 0) {
      if (errno == EINTR) continue;  // interrupted by a signal: just loop again
      LOG_ERROR("epoll_wait: %s", std::strerror(errno));
      break;
    }

    for (int i = 0; i < n; ++i) {
      Handle* h = static_cast<Handle*>(events[i].data.ptr);
      if (h->conn == nullptr) {
        accept_ready();  // event on the listening socket = new client(s)
      } else {
        handle_event(h, events[i].events);
      }
    }

    // Safe point: the batch is finished, nobody holds pointers to closed
    // connections any more, so free them now.
    for (Connection* c : graveyard_) delete c;
    graveyard_.clear();

    if (now_ms_ - last_sweep_ms_ >= 1000) {
      last_sweep_ms_ = now_ms_;
      sweep_timeouts();
      for (Connection* c : graveyard_) delete c;  // sweep may have closed some
      graveyard_.clear();
    }
  }
}

// ---------------------------------------------------------------------------
// accept_ready: take new clients off the listening socket
// ---------------------------------------------------------------------------
void Worker::accept_ready() {
  for (int i = 0; i < kAcceptBatch; ++i) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    // accept4 = accept + set flags atomically. The new socket is non-blocking.
    int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len,
                       SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // nobody else is waiting
      // EMFILE = "too many open files": raise `ulimit -n`. We just stop for now.
      LOG_WARN("accept: %s", std::strerror(errno));
      break;
    }
    util::set_tcp_nodelay(fd);

    Connection* c = new Connection();
    c->client.fd = fd;
    c->last_activity_ms = now_ms_;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));  // binary address -> "1.2.3.4"
    c->client_ip = ip;
    conns_.insert(c);

    begin_connect(c);
  }
}

// ---------------------------------------------------------------------------
// begin_connect: pick a backend and start a NON-BLOCKING connect() to it
// ---------------------------------------------------------------------------
void Worker::begin_connect(Connection* c) {
  // Ask the balancer. tried_mask excludes backends that already failed for this client.
  Backend* b = balancer_.acquire(c->client_ip, c->tried_mask);
  if (b == nullptr) {
    reject(c);  // every backend is down or already tried
    return;
  }
  c->backend_ptr = b;  // from now on release_backend() will undo the active_conns++
  c->attempts++;

  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    LOG_ERROR("socket(): %s", std::strerror(errno));
    release_backend(c);
    reject(c);
    return;
  }
  util::set_tcp_nodelay(fd);
  c->backend.fd = fd;
  c->backend.in_epoll = false;
  c->backend.events = 0;
  c->state = Connection::State::Connecting;
  c->connect_started_ms = now_ms_;

  int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&b->addr), sizeof(b->addr));
  if (rc == 0 || errno == EINPROGRESS) {
    // EINPROGRESS is the NORMAL result for a non-blocking connect: "working on it".
    // We register the fd for EPOLLOUT; epoll tells us when the attempt is over.
    // (Even if rc == 0, the writable event arrives right away, so one code path.)
    update_interest(c);
  } else {
    on_connect_failed(c, errno);  // e.g. network unreachable: failed immediately
  }
}

// ---------------------------------------------------------------------------
// on_connect_event: the backend socket became writable => connect() has finished
// ---------------------------------------------------------------------------
void Worker::on_connect_event(Connection* c) {
  // "Writable" does not say whether it worked. SO_ERROR holds the verdict.
  int err = 0;
  socklen_t len = sizeof(err);
  getsockopt(c->backend.fd, SOL_SOCKET, SO_ERROR, &err, &len);
  if (err != 0) {
    on_connect_failed(c, err);
    return;
  }

  c->state = Connection::State::Established;
  c->last_activity_ms = now_ms_;
  c->backend_ptr->breaker.on_success();
  c->backend_ptr->total_conns.fetch_add(1, std::memory_order_relaxed);
  LOG_DEBUG("%s -> %s connected", c->client_ip.c_str(), c->backend_ptr->name.c_str());

  // The client may have sent its request while we were connecting; it is waiting
  // in c2b. Send it now.
  flush(c, Side::Backend);
  if (c->closed) return;
  check_finish(c);
  if (c->closed) return;
  update_interest(c);
}

// ---------------------------------------------------------------------------
// on_connect_failed: tell the breaker, forget this backend, try a different one
// ---------------------------------------------------------------------------
void Worker::on_connect_failed(Connection* c, int err) {
  Backend* b = c->backend_ptr;
  if (b != nullptr) {
    LOG_WARN("connect to %s failed: %s", b->name.c_str(), std::strerror(err));
    b->breaker.on_failure();
    b->total_failures.fetch_add(1, std::memory_order_relaxed);
    c->tried_mask |= (1ULL << b->index);  // never retry this one for THIS client
  }
  release_backend(c);

  if (c->attempts >= cfg_.max_retries) {
    reject(c);
    return;
  }
  // Failover: the client never noticed anything, and its request bytes are
  // still sitting in c2b. Just connect to another backend.
  begin_connect(c);
}

void Worker::release_backend(Connection* c) {
  if (c->backend.fd >= 0) {
    ::close(c->backend.fd);  // closing an fd automatically removes it from epoll
    c->backend.fd = -1;
    c->backend.in_epoll = false;
    c->backend.events = 0;
  }
  if (c->backend_ptr != nullptr) {
    c->backend_ptr->active_conns.fetch_sub(1, std::memory_order_relaxed);
    c->backend_ptr = nullptr;
  }
}

void Worker::reject(Connection* c) {
  ::send(c->client.fd, kNoBackend, sizeof(kNoBackend) - 1, MSG_NOSIGNAL);  // best effort
  close_connection(c);
}

// ---------------------------------------------------------------------------
// handle_event: dispatch one epoll event for one socket
// ---------------------------------------------------------------------------
void Worker::handle_event(Handle* h, uint32_t events) {
  Connection* c = h->conn;
  // Stale event: this connection (or this fd) was closed earlier in the same batch.
  if (c->closed || h->fd < 0) return;
  const Side side = h->side;

  // While connecting, the ONLY thing we care about on the backend fd is
  // "did connect() finish?"; any event means yes (success or failure).
  if (side == Side::Backend && c->state == Connection::State::Connecting) {
    on_connect_event(c);
    return;
  }

  if (events & EPOLLERR) {  // socket error (e.g. connection reset)
    close_connection(c);
    return;
  }

  // EPOLLHUP is treated like "readable": calling recv() will drain remaining data
  // and then return 0 (end of stream), which is how we learn the peer is gone.
  if (events & (EPOLLIN | EPOLLHUP)) {
    on_readable(c, side);
    if (c->closed) return;
  }
  // The client fully hung up and we already know it: nobody can receive our reply.
  if ((events & EPOLLHUP) && side == Side::Client && c->client_eof) {
    close_connection(c);
    return;
  }
  if (events & EPOLLOUT) {  // the socket can accept more of our data
    flush(c, side);
    if (c->closed) return;
  }

  check_finish(c);
  if (c->closed) return;
  update_interest(c);
}

// ---------------------------------------------------------------------------
// on_readable: read from one socket into its buffer, then try to forward it
// ---------------------------------------------------------------------------
void Worker::on_readable(Connection* c, Side side) {
  Buffer& buf = (side == Side::Client) ? c->c2b : c->b2c;
  bool& eof = (side == Side::Client) ? c->client_eof : c->backend_eof;
  Handle& h = (side == Side::Client) ? c->client : c->backend;

  if (eof) return;
  if (buf.size() >= kMaxBuffer) return;  // backpressure: buffer full, stop reading for now

  size_t room = kMaxBuffer - buf.size();
  ssize_t n = ::recv(h.fd, scratch_.data(), std::min(room, scratch_.size()), 0);

  if (n > 0) {
    buf.append(scratch_.data(), static_cast<size_t>(n));
    c->last_activity_ms = now_ms_;
    // Immediately try to push it to the other side (usually succeeds at once).
    flush(c, side == Side::Client ? Side::Backend : Side::Client);
  } else if (n == 0) {
    eof = true;  // the peer closed its sending direction ("FIN" received)
  } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    // Nothing to read after all. Fine.
  } else {
    close_connection(c);  // real error such as ECONNRESET
  }
}

// ---------------------------------------------------------------------------
// flush: write as much of a buffer as the destination socket will take
// ---------------------------------------------------------------------------
// `to` is the side we are WRITING TO. Backend => drain c2b; Client => drain b2c.
void Worker::flush(Connection* c, Side to) {
  if (to == Side::Backend && c->state != Connection::State::Established) return;  // not yet

  Buffer& buf = (to == Side::Backend) ? c->c2b : c->b2c;
  Handle& h = (to == Side::Backend) ? c->backend : c->client;

  while (!buf.empty()) {
    // MSG_NOSIGNAL: if the peer is gone, return EPIPE instead of killing the
    // whole process with SIGPIPE.
    ssize_t n = ::send(h.fd, buf.data(), buf.size(), MSG_NOSIGNAL);
    if (n > 0) {
      buf.consume(static_cast<size_t>(n));
      c->last_activity_ms = now_ms_;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;  // kernel send buffer is full. update_interest() will ask for EPOLLOUT.
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      close_connection(c);  // EPIPE / ECONNRESET ...
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// check_finish: decide whether the connection (or half of it) is over
// ---------------------------------------------------------------------------
void Worker::check_finish(Connection* c) {
  // Backend finished talking and everything it said has reached the client
  // => the exchange is complete. Close both sides.
  if (c->backend_eof && c->b2c.empty()) {
    close_connection(c);
    return;
  }

  // Client finished talking and the connect is still pending with nothing to send.
  if (c->client_eof && c->c2b.empty() && c->state == Connection::State::Connecting) {
    close_connection(c);
    return;
  }

  // Client finished sending and all its bytes were delivered: pass the "end of
  // request" along with shutdown(SHUT_WR) (a half-close). The backend can still
  // send its reply back to the client.
  if (c->client_eof && c->c2b.empty() && c->state == Connection::State::Established &&
      !c->backend_shut_wr) {
    ::shutdown(c->backend.fd, SHUT_WR);
    c->backend_shut_wr = true;
  }
}

// ---------------------------------------------------------------------------
// update_interest: choose which events epoll should report for each socket
// ---------------------------------------------------------------------------
// We use LEVEL-TRIGGERED epoll: "keep telling me as long as the condition is
// true". It is forgiving: if we do not read everything now, epoll reminds us next
// loop. The catch: if a condition stays true and we do not act on it (e.g. socket
// readable but our buffer is full), epoll would wake us again and again (a "busy
// loop"). So we only subscribe to events we are actually ready to handle.
void Worker::update_interest(Connection* c) {
  if (c->closed) return;

  // --- client socket ---
  uint32_t cm = 0;
  if (!c->client_eof && c->c2b.size() < kMaxBuffer) cm |= EPOLLIN;  // room to read more
  if (!c->b2c.empty()) cm |= EPOLLOUT;                              // data waiting to be sent
  set_interest(c->client, cm);
  if (c->closed) return;

  // --- backend socket ---
  if (c->backend.fd < 0) return;
  if (c->state == Connection::State::Connecting) {
    set_interest(c->backend, EPOLLOUT);  // writable == connect finished
  } else if (c->backend_eof) {
    // The backend has said everything it will say; we no longer need events from
    // it. Removing it also avoids a busy loop on a permanent EPOLLHUP.
    remove_from_epoll(c->backend);
  } else {
    uint32_t bm = 0;
    if (c->b2c.size() < kMaxBuffer) bm |= EPOLLIN;
    if (!c->c2b.empty()) bm |= EPOLLOUT;
    set_interest(c->backend, bm);
  }
}

void Worker::set_interest(Handle& h, uint32_t events) {
  if (h.fd < 0) return;
  if (h.in_epoll && h.events == events) return;  // nothing changed: skip the syscall

  epoll_event ev{};
  ev.events = events;
  ev.data.ptr = &h;
  int op = h.in_epoll ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if (::epoll_ctl(epfd_, op, h.fd, &ev) < 0) {
    LOG_ERROR("epoll_ctl: %s", std::strerror(errno));
    close_connection(h.conn);
    return;
  }
  h.in_epoll = true;
  h.events = events;
}

void Worker::remove_from_epoll(Handle& h) {
  if (h.fd >= 0 && h.in_epoll) {
    ::epoll_ctl(epfd_, EPOLL_CTL_DEL, h.fd, nullptr);
    h.in_epoll = false;
    h.events = 0;
  }
}

// ---------------------------------------------------------------------------
// close_connection: tear everything down (idempotent: safe to call twice)
// ---------------------------------------------------------------------------
void Worker::close_connection(Connection* c) {
  if (c->closed) return;
  c->closed = true;

  release_backend(c);  // closes backend fd and decrements the backend's counter
  if (c->client.fd >= 0) {
    ::close(c->client.fd);
    c->client.fd = -1;
    c->client.in_epoll = false;
  }
  conns_.erase(c);
  graveyard_.push_back(c);  // delete later, see run()
}

// ---------------------------------------------------------------------------
// sweep_timeouts: once per second look for stuck connections
// ---------------------------------------------------------------------------
void Worker::sweep_timeouts() {
  // Copy first: closing connections modifies conns_ and would break iteration.
  std::vector<Connection*> snapshot(conns_.begin(), conns_.end());
  for (Connection* c : snapshot) {
    if (c->closed) continue;
    if (c->state == Connection::State::Connecting &&
        now_ms_ - c->connect_started_ms > cfg_.connect_timeout_ms) {
      on_connect_failed(c, ETIMEDOUT);  // counts against the backend, then retries elsewhere
    } else if (now_ms_ - c->last_activity_ms > cfg_.idle_timeout_ms) {
      LOG_DEBUG("idle timeout: closing connection from %s", c->client_ip.c_str());
      close_connection(c);
    }
  }
}
