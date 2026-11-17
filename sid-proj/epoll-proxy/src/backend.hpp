// ===========================================================================
// backend.hpp  --  one upstream server that we forward traffic to
// ===========================================================================
// Many threads read and update these fields at the same time:
//   * worker threads: active_conns, total_conns, breaker
//   * health-check threads: healthy
//   * main thread: reads everything for the stats table
// Plain ints would be a "data race" (undefined behaviour). std::atomic makes
// each individual read/write indivisible and visible to every thread.
#pragma once
#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "circuit_breaker.hpp"

struct Backend {
  Backend(int idx, std::string display_name, sockaddr_in address, int breaker_threshold,
          int breaker_cooldown_ms)
      : index(idx),
        name(std::move(display_name)),
        addr(address),
        breaker(name, breaker_threshold, breaker_cooldown_ms) {}

  // Atomics cannot be copied, so a Backend must not be copied either.
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  const int index;          // position in the backend list (0,1,2...). Used as a bit number.
  const std::string name;   // "127.0.0.1:9001", for logs
  const sockaddr_in addr;   // numeric address used by connect()

  std::atomic<int> active_conns{0};        // proxied connections open right now
  std::atomic<bool> healthy{true};         // set by the active health checker
  std::atomic<uint64_t> total_conns{0};    // lifetime successful connections (stats)
  std::atomic<uint64_t> total_failures{0}; // lifetime failed connects (stats)
  CircuitBreaker breaker;                  // passive failure protection

  // Can we send NEW traffic here? Both the health checker AND the breaker must agree.
  bool is_available() const {
    return healthy.load(std::memory_order_acquire) && breaker.is_available();
  }
};
