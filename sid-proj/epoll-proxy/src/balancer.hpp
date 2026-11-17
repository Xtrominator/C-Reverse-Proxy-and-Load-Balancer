// ===========================================================================
// balancer.hpp  --  "which backend should this new client connection go to?"
// ===========================================================================
// Three strategies, all behind the same interface:
//
//   rr      Round robin.   A, B, C, A, B, C ...  Simple and fair when every
//                          connection costs about the same.
//   lc      Least connections.  Pick the backend with the fewest connections open
//                          right now. Great when connections last different
//                          lengths of time (a slow backend piles up connections,
//                          so it automatically gets fewer new ones).
//   ketama  Consistent hashing on the client IP. The same client keeps landing
//                          on the same backend ("sticky"), and when one backend
//                          dies only ITS clients move, everyone else stays put.
//
// "exclude_mask" is a bit set: bit i set means "do not pick backend i" (we
// already tried it and it failed). That is why we allow at most 64 backends.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backend.hpp"

class Balancer {
 public:
  explicit Balancer(std::vector<Backend*> backends) : backends_(std::move(backends)) {}
  virtual ~Balancer() = default;

  // Choose a backend AND reserve it (active_conns++, breaker permission).
  // Returns nullptr if nothing usable exists. The caller MUST later do
  // active_conns-- when it is done with the backend.
  // Thread-safe: every worker thread calls this concurrently.
  Backend* acquire(const std::string& client_key, uint64_t exclude_mask);

  virtual const char* name() const = 0;

 protected:
  // Strategy-specific choice. Must not modify any state except its own counters.
  virtual Backend* pick(const std::string& client_key, uint64_t exclude_mask) = 0;

  // Is this backend a legal candidate right now?
  static bool usable(const Backend* b, uint64_t exclude_mask) {
    return !((exclude_mask >> b->index) & 1ULL) && b->is_available();
  }

  std::vector<Backend*> backends_;  // non-owning pointers; main() owns the Backends
};

// algo is "rr", "lc" or "ketama". Returns nullptr for an unknown name.
std::unique_ptr<Balancer> make_balancer(const std::string& algo, std::vector<Backend*> backends);

// Exposed so the self-test in main.cpp can poke at the ring without sockets.
// Ketama needs a stable number for a string; this is "first 4 bytes of MD5".
uint32_t ketama_hash(const std::string& key);
