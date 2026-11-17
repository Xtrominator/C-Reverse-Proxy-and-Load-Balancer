// ===========================================================================
// util.hpp  --  tiny helpers used everywhere: a clock, a socket option, logging
// ===========================================================================
#pragma once
#include <cstdint>

namespace util {

// Milliseconds from a "stopwatch" clock that only moves forward.
// We use steady_clock (NOT the wall clock) for timeouts, because the wall
// clock can jump backwards when someone changes the system time.
int64_t now_ms();

// Disable Nagle's algorithm on a TCP socket.
// By default TCP waits a little to merge small writes into bigger packets.
// A proxy wants data forwarded immediately, so we turn that waiting off.
void set_tcp_nodelay(int fd);

}  // namespace util

// ---- Logging ---------------------------------------------------------------
enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_log_level(LogLevel level);

// printf-style logging. It is thread-safe: several threads may log at once.
// The attribute below makes the compiler check the format string for us.
void log_msg(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_DEBUG(...) log_msg(LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  log_msg(LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)  log_msg(LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LogLevel::Error, __VA_ARGS__)
