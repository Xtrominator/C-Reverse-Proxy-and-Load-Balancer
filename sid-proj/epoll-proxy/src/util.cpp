#include "util.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace util {

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void set_tcp_nodelay(int fd) {
  int one = 1;
  // setsockopt(socket, protocol level, option name, pointer to value, size)
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

}  // namespace util

// ---- Logging ---------------------------------------------------------------
// The current minimum level. std::atomic lets any thread read/write it safely.
static std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
// Without a lock, two threads printing at once could interleave their text.
static std::mutex g_log_mutex;

void set_log_level(LogLevel level) { g_level.store(static_cast<int>(level)); }

void log_msg(LogLevel level, const char* fmt, ...) {
  if (static_cast<int>(level) < g_level.load()) return;  // too quiet to print

  // 1) Format the user's message into a local buffer.
  char msg[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  // 2) Build a timestamp like 14:03:59
  char ts[32];
  std::time_t t = std::time(nullptr);
  std::tm tm_buf;
  localtime_r(&t, &tm_buf);  // the _r version is thread-safe
  std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

  static const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};

  // 3) Print while holding the lock so lines never get mixed together.
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::fprintf(stderr, "[%s] [%s] %s\n", ts, names[static_cast<int>(level)], msg);
}
