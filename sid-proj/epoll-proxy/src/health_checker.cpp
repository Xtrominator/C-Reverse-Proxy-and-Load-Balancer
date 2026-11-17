#include "health_checker.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "util.hpp"

namespace {

// Wait up to timeout_ms until `fd` is ready for `events` (POLLIN / POLLOUT).
// Returns true if it became ready in time.
bool wait_for(int fd, short events, int timeout_ms) {
  pollfd p{};
  p.fd = fd;
  p.events = events;
  int rc;
  do {
    rc = ::poll(&p, 1, timeout_ms);
  } while (rc < 0 && errno == EINTR);  // a signal interrupted us: just retry
  return rc > 0;
}

}  // namespace

HealthChecker::HealthChecker(std::vector<Backend*> backends, const Config& cfg)
    : backends_(std::move(backends)), cfg_(cfg) {}

void HealthChecker::start() {
  for (Backend* b : backends_) threads_.emplace_back([this, b] { run_for(b); });
}

void HealthChecker::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();  // wake every sleeping probe thread
  for (auto& t : threads_)
    if (t.joinable()) t.join();
  threads_.clear();
}

bool HealthChecker::probe(const Backend& backend) const {
  // A normal TCP socket, but non-blocking so that we control the timeout ourselves
  // (a blocking connect() to a black-holed address can hang for over a minute).
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;

  bool ok = false;
  do {
    // --- 1. connect (with timeout) ---
    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&backend.addr), sizeof(backend.addr));
    if (rc < 0 && errno != EINPROGRESS) break;  // refused immediately, etc.
    if (rc < 0) {
      // Connection is "in progress". The socket becomes WRITABLE when it finishes
      // (successfully or not), then SO_ERROR tells us which.
      if (!wait_for(fd, POLLOUT, cfg_.health_timeout_ms)) break;
      int err = 0;
      socklen_t len = sizeof(err);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
      if (err != 0) break;
    }

    // --- 2. if no path configured, a successful TCP connect is enough ---
    if (cfg_.health_path.empty()) {
      ok = true;
      break;
    }

    // --- 3. send a tiny HTTP request ---
    std::string req = "GET " + cfg_.health_path +
                      " HTTP/1.0\r\nHost: health-check\r\nConnection: close\r\n\r\n";
    if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(req.size())) break;

    // --- 4. read the start of the reply and look at the status code ---
    if (!wait_for(fd, POLLIN, cfg_.health_timeout_ms)) break;
    char buf[64];
    ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n < 12) break;
    buf[n] = '\0';
    // Expect "HTTP/1.x 2xx ..." : byte 9 is the first digit of the status code.
    ok = (std::strncmp(buf, "HTTP/1.", 7) == 0 && buf[9] == '2');
  } while (false);  // "do { } while(false)" lets us use `break` as an early exit

  ::close(fd);
  return ok;
}

void HealthChecker::run_for(Backend* b) {
  int consecutive_ok = 0, consecutive_fail = 0;

  std::unique_lock<std::mutex> lock(mu_);
  while (!stopping_) {
    lock.unlock();               // do not hold the lock while talking to the network
    const bool ok = probe(*b);
    lock.lock();

    if (ok) {
      consecutive_ok++;
      consecutive_fail = 0;
      if (!b->healthy.load() && consecutive_ok >= cfg_.health_rise) {
        b->healthy.store(true, std::memory_order_release);
        b->breaker.reset();  // health check proved it is alive: let traffic in again
        LOG_INFO("health: %s is UP", b->name.c_str());
      }
    } else {
      consecutive_fail++;
      consecutive_ok = 0;
      if (b->healthy.load() && consecutive_fail >= cfg_.health_fall) {
        b->healthy.store(false, std::memory_order_release);
        LOG_WARN("health: %s is DOWN (%d failed probes)", b->name.c_str(), consecutive_fail);
      }
    }

    // Sleep, but wake immediately if stop() is called.
    cv_.wait_for(lock, std::chrono::milliseconds(cfg_.health_interval_ms),
                 [this] { return stopping_; });
  }
}
