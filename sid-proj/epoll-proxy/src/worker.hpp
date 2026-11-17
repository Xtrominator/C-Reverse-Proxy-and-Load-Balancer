// ===========================================================================
// worker.hpp  --  one thread running one epoll event loop
// ===========================================================================
// BIG PICTURE
//
// A naive server uses one thread per connection. 10,000 connections would mean
// 10,000 threads: way too much memory and context switching.
//
// An EVENT-DRIVEN server uses a few threads. Each thread owns an "epoll
// instance": you register many sockets with it, then call epoll_wait(), which
// sleeps until at least one socket has something to do ("readable", "writable",
// "error") and then returns just the sockets that are ready. The thread handles
// each ready socket quickly WITHOUT ever blocking, then calls epoll_wait again.
//
// For this to work every socket is NON-BLOCKING: read()/write()/connect() return
// immediately (with errno EAGAIN / EINPROGRESS) instead of waiting.
//
// HOW THREADS SHARE THE WORK
//
// We start N workers. Each worker creates its OWN listening socket on the same
// port using SO_REUSEPORT; the Linux kernel spreads new connections across those
// sockets. Result: no shared accept queue, no locks, and each connection lives
// entirely inside ONE thread. The only shared data are the atomics/mutex inside
// Backend, CircuitBreaker and Balancer.
#pragma once
#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>

#include "balancer.hpp"
#include "config.hpp"
#include "connection.hpp"

// Set to true by the SIGINT/SIGTERM handler; every loop checks it to shut down.
extern std::atomic<bool> g_stop;

class Worker {
 public:
  Worker(int id, const Config& cfg, Balancer& balancer);
  ~Worker();

  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  bool init();  // create epoll + listening socket; false on error
  void run();   // the event loop; returns when g_stop becomes true

 private:
  // ---- accepting ----
  void accept_ready();

  // ---- backend connection management ----
  void begin_connect(Connection* c);                  // choose a backend, start connect()
  void on_connect_event(Connection* c);               // epoll says connect() finished
  void on_connect_failed(Connection* c, int err);     // failed: breaker + retry elsewhere
  void release_backend(Connection* c);                // close backend fd, active_conns--
  void reject(Connection* c);                         // reply 503 and close

  // ---- data pumping ----
  void handle_event(Handle* h, uint32_t events);
  void on_readable(Connection* c, Side side);         // recv() from a socket into a buffer
  void flush(Connection* c, Side to);                 // send() a buffer to a socket
  void check_finish(Connection* c);                   // half-close / full-close logic
  void update_interest(Connection* c);                // tell epoll what we want to hear about
  void set_interest(Handle& h, uint32_t events);
  void remove_from_epoll(Handle& h);

  // ---- teardown ----
  void close_connection(Connection* c);
  void sweep_timeouts();

  int id_;
  const Config& cfg_;
  Balancer& balancer_;

  int epfd_ = -1;         // the epoll instance
  int listen_fd_ = -1;    // this worker's own listening socket
  Handle listen_handle_;  // handle with conn == nullptr marks "the listener"

  std::unordered_set<Connection*> conns_;  // every live connection owned by this worker

  // Connections closed while processing a batch of events are NOT deleted at once:
  // later events in the same batch may still point at them. We delete them after
  // the batch, when nothing can reference them any more.
  std::vector<Connection*> graveyard_;

  std::vector<char> scratch_;  // temporary buffer recv() writes into
  int64_t now_ms_ = 0;         // cached clock, refreshed once per loop iteration
  int64_t last_sweep_ms_ = 0;
};
