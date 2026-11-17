// ===========================================================================
// main.cpp  --  program entry point: read options, build everything, run
// ===========================================================================
//
// Startup order:
//   1. parse command-line options           -> Config
//   2. raise the open-file limit            (each proxied connection = 2 sockets)
//   3. resolve backend addresses            -> Backend objects
//   4. create the Balancer (rr / lc / ketama)
//   5. start the health-check threads
//   6. create N Workers (each with its own epoll + listening socket) and run each
//      one in its own thread
//   7. main thread idles (optionally printing stats) until Ctrl+C
//   8. stop everything and exit
#include <netdb.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "backend.hpp"
#include "balancer.hpp"
#include "config.hpp"
#include "health_checker.hpp"
#include "md5.hpp"
#include "util.hpp"
#include "worker.hpp"

// Defined here, declared extern in worker.hpp. Set by the signal handler.
std::atomic<bool> g_stop{false};

namespace {

extern "C" void on_signal(int) { g_stop.store(true); }  // keep signal handlers tiny!

void print_usage(const char* prog) {
  std::printf(
      "Usage: %s [options] --backend HOST:PORT [--backend HOST:PORT ...]\n\n"
      "  --port N              listen port (default 8080)\n"
      "  --threads N           worker threads (default: number of CPU cores)\n"
      "  --algo rr|lc|ketama   balancing algorithm (default lc)\n"
      "  --backend HOST:PORT   an upstream server (repeat for several, max 64)\n"
      "  --health-path PATH    HTTP path to probe (default /health, \"\" = TCP connect only)\n"
      "  --health-interval MS  pause between probes (default 500)\n"
      "  --no-health-check     disable active health checks\n"
      "  --connect-timeout MS  backend connect timeout (default 1000)\n"
      "  --idle-timeout MS     close silent connections after this (default 60000)\n"
      "  --stats               print per-backend statistics every 5 s\n"
      "  --debug               verbose logging\n"
      "  --self-test           run built-in unit checks and exit\n",
      prog);
}

// Parse "--flag value" style options. Returns false on a bad option.
bool parse_args(int argc, char** argv, Config& cfg, bool& self_test) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    // Helper lambda: fetch the value that follows an option.
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", what);
        return nullptr;
      }
      return argv[++i];
    };
    const char* v = nullptr;
    if (a == "--port")                { if (!(v = next("--port"))) return false; cfg.port = static_cast<uint16_t>(std::atoi(v)); }
    else if (a == "--threads")        { if (!(v = next("--threads"))) return false; cfg.threads = std::atoi(v); }
    else if (a == "--algo")           { if (!(v = next("--algo"))) return false; cfg.algo = v; }
    else if (a == "--backend")        { if (!(v = next("--backend"))) return false; cfg.backends.push_back(v); }
    else if (a == "--health-path")    { if (!(v = next("--health-path"))) return false; cfg.health_path = v; }
    else if (a == "--health-interval"){ if (!(v = next("--health-interval"))) return false; cfg.health_interval_ms = std::atoi(v); }
    else if (a == "--connect-timeout"){ if (!(v = next("--connect-timeout"))) return false; cfg.connect_timeout_ms = std::atoi(v); }
    else if (a == "--idle-timeout")   { if (!(v = next("--idle-timeout"))) return false; cfg.idle_timeout_ms = std::atoi(v); }
    else if (a == "--no-health-check") cfg.health_enabled = false;
    else if (a == "--stats")           cfg.stats = true;
    else if (a == "--debug")           cfg.debug = true;
    else if (a == "--self-test")       self_test = true;
    else if (a == "--help" || a == "-h") return false;
    else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return false; }
  }
  return true;
}

// Turn "host:port" into a sockaddr_in. getaddrinfo also understands DNS names
// like "backend1" (needed inside Docker, where hosts are found by name).
bool resolve_backend(const std::string& spec, sockaddr_in& out) {
  size_t colon = spec.rfind(':');
  if (colon == std::string::npos) return false;
  std::string host = spec.substr(0, colon), port = spec.substr(colon + 1);

  addrinfo hints{};
  hints.ai_family = AF_INET;        // IPv4 only (keeps the code simple)
  hints.ai_socktype = SOCK_STREAM;  // TCP
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) return false;
  std::memcpy(&out, res->ai_addr, sizeof(out));
  freeaddrinfo(res);
  return true;
}

void raise_fd_limit() {
  // Every proxied connection needs TWO file descriptors (client + backend). The
  // default limit (often 1024) would cap us at ~500 connections.
  rlimit rl{};
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
    LOG_INFO("open file limit: %llu (=> about %llu proxied connections)",
             static_cast<unsigned long long>(rl.rlim_cur),
             static_cast<unsigned long long>(rl.rlim_cur / 2));
}

void print_stats(const std::vector<std::unique_ptr<Backend>>& backends) {
  std::fprintf(stderr, "---- backend stats ----\n");
  for (const auto& b : backends) {
    std::fprintf(stderr, "  %-22s %-5s breaker=%-9s active=%-6d total=%-10llu connect_failures=%llu\n",
                 b->name.c_str(), b->healthy.load() ? "UP" : "DOWN",
                 CircuitBreaker::state_name(b->breaker.state()), b->active_conns.load(),
                 static_cast<unsigned long long>(b->total_conns.load()),
                 static_cast<unsigned long long>(b->total_failures.load()));
  }
}

// ---------------------------------------------------------------------------
// --self-test: a few checks that need no network. Great for learning too.
// ---------------------------------------------------------------------------
int run_self_test() {
  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
  };

  // 1) MD5 against the official test vectors from RFC 1321.
  check(to_hex(md5("")) == "d41d8cd98f00b204e9800998ecf8427e", "md5(\"\")");
  check(to_hex(md5("abc")) == "900150983cd24fb0d6963f7d28e17f72", "md5(\"abc\")");
  check(to_hex(md5("The quick brown fox jumps over the lazy dog")) ==
            "9e107d9d372bb6826bd81d3542a419d6", "md5(quick brown fox)");

  // 2) Ketama: same key -> same backend; killing one backend only moves ITS keys.
  sockaddr_in dummy{};
  std::vector<std::unique_ptr<Backend>> owned;
  std::vector<Backend*> ptrs;
  for (int i = 0; i < 4; ++i) {
    owned.push_back(std::make_unique<Backend>(i, "10.0.0." + std::to_string(i + 1) + ":80", dummy, 3, 1000));
    ptrs.push_back(owned.back().get());
  }
  auto ring = make_balancer("ketama", ptrs);

  const int kKeys = 4000;
  std::vector<int> before(kKeys);
  std::map<int, int> spread;
  bool stable = true;
  for (int k = 0; k < kKeys; ++k) {
    std::string ip = "192.168." + std::to_string(k / 250) + "." + std::to_string(k % 250);
    Backend* a = ring->acquire(ip, 0);
    Backend* b = ring->acquire(ip, 0);
    before[k] = a->index;
    if (a != b) stable = false;
    spread[a->index]++;
    a->active_conns--; b->active_conns--;
  }
  check(stable, "ketama: same client IP always maps to the same backend");
  bool even = true;
  for (auto& [idx, cnt] : spread) if (cnt < kKeys / 4 / 2 || cnt > kKeys / 4 * 2) even = false;
  check(even, "ketama: keys are spread roughly evenly over 4 backends");

  owned[2]->healthy = false;  // "kill" backend #2
  int moved_wrongly = 0, moved_total = 0;
  for (int k = 0; k < kKeys; ++k) {
    std::string ip = "192.168." + std::to_string(k / 250) + "." + std::to_string(k % 250);
    Backend* a = ring->acquire(ip, 0);
    if (a->index != before[k]) {
      ++moved_total;
      if (before[k] != 2) ++moved_wrongly;  // a key moved although its backend was alive
    }
    a->active_conns--;
  }
  std::printf("      (%d of %d keys moved after backend #2 died)\n", moved_total, kKeys);
  check(moved_wrongly == 0 && moved_total > 0, "ketama: only the dead backend's keys are re-routed");

  // 3) Round robin spreads evenly; least-connections prefers the idle backend.
  owned[2]->healthy = true;
  auto rr = make_balancer("rr", ptrs);
  std::map<int, int> rr_count;
  for (int i = 0; i < 400; ++i) { Backend* b = rr->acquire("x", 0); rr_count[b->index]++; b->active_conns--; }
  check(rr_count[0] == 100 && rr_count[1] == 100 && rr_count[2] == 100 && rr_count[3] == 100,
        "round-robin: 400 picks give exactly 100 per backend");

  auto lc = make_balancer("lc", ptrs);
  owned[0]->active_conns = 10; owned[1]->active_conns = 10; owned[2]->active_conns = 1; owned[3]->active_conns = 10;
  Backend* pick = lc->acquire("x", 0);
  check(pick->index == 2, "least-connections: chooses the backend with the fewest connections");

  // 4) Circuit breaker walks through its states.
  CircuitBreaker cb("test", 3, 50);
  cb.on_failure(); cb.on_failure();
  check(cb.state() == CircuitBreaker::State::Closed, "breaker: 2 failures, still closed");
  cb.on_failure();
  check(cb.state() == CircuitBreaker::State::Open && !cb.is_available(), "breaker: 3rd failure opens it");
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  check(cb.is_available() && cb.try_acquire() && cb.state() == CircuitBreaker::State::HalfOpen,
        "breaker: after cooldown one trial is allowed (half-open)");
  check(!cb.try_acquire(), "breaker: a second trial at the same time is refused");
  cb.on_success();
  check(cb.state() == CircuitBreaker::State::Closed, "breaker: successful trial closes it");

  std::printf("%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  bool self_test = false;
  if (!parse_args(argc, argv, cfg, self_test)) {
    print_usage(argv[0]);
    return 2;
  }
  if (self_test) return run_self_test();

  if (cfg.backends.empty()) {
    std::fprintf(stderr, "error: at least one --backend is required\n\n");
    print_usage(argv[0]);
    return 2;
  }
  if (cfg.backends.size() > 64) {
    std::fprintf(stderr, "error: at most 64 backends are supported\n");
    return 2;
  }
  if (cfg.debug) set_log_level(LogLevel::Debug);
  if (cfg.threads <= 0) cfg.threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

  // Do not die if a client vanishes mid-write. (We also pass MSG_NOSIGNAL to send().)
  signal(SIGPIPE, SIG_IGN);
  // Ctrl+C (SIGINT) and `docker stop` (SIGTERM) request a clean shutdown.
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  raise_fd_limit();

  // ---- Backends ----
  std::vector<std::unique_ptr<Backend>> backends;  // main() OWNS them
  std::vector<Backend*> backend_ptrs;              // everyone else just borrows
  for (const std::string& spec : cfg.backends) {
    sockaddr_in addr{};
    if (!resolve_backend(spec, addr)) {
      std::fprintf(stderr, "error: cannot resolve backend '%s' (expected HOST:PORT)\n", spec.c_str());
      return 2;
    }
    backends.push_back(std::make_unique<Backend>(static_cast<int>(backends.size()), spec, addr,
                                                 cfg.breaker_threshold, cfg.breaker_cooldown_ms));
    backend_ptrs.push_back(backends.back().get());
  }

  // ---- Balancer ----
  std::unique_ptr<Balancer> balancer = make_balancer(cfg.algo, backend_ptrs);
  if (!balancer) {
    std::fprintf(stderr, "error: unknown --algo '%s' (use rr, lc or ketama)\n", cfg.algo.c_str());
    return 2;
  }

  // ---- Workers: create + init all BEFORE starting threads, so errors show up early ----
  std::vector<std::unique_ptr<Worker>> workers;
  for (int i = 0; i < cfg.threads; ++i) {
    workers.push_back(std::make_unique<Worker>(i, cfg, *balancer));
    if (!workers.back()->init()) return 1;
  }

  LOG_INFO("listening on port %d | %d worker threads | algorithm: %s", cfg.port, cfg.threads,
           balancer->name());
  for (Backend* b : backend_ptrs) LOG_INFO("backend #%d: %s", b->index, b->name.c_str());

  // ---- Health checks ----
  HealthChecker health(backend_ptrs, cfg);
  if (cfg.health_enabled) health.start();

  // ---- Start worker threads ----
  std::vector<std::thread> threads;
  for (auto& w : workers) threads.emplace_back([&w] { w->run(); });

  // ---- Main thread: wait for Ctrl+C, optionally print stats ----
  int ticks = 0;
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (cfg.stats && ++ticks % 50 == 0) print_stats(backends);
  }

  LOG_INFO("shutting down...");
  for (auto& t : threads) t.join();  // workers notice g_stop within 200 ms
  health.stop();
  return 0;
}
