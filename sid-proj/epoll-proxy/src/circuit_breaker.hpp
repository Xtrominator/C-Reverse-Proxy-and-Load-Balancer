// ===========================================================================
// circuit_breaker.hpp  --  stop hammering a backend that keeps failing
// ===========================================================================
// Idea (borrowed from electrical fuses):
//
//      +--------+  N failures in a row   +------+
//      | CLOSED | ---------------------> | OPEN |   (no traffic allowed)
//      | normal |                        +------+
//      +--------+                           |
//         ^                                 | cooldown time passes
//         | trial succeeds                  v
//         |                           +-----------+
//         +-------------------------- | HALF-OPEN | (let ONE trial through)
//                                     +-----------+
//                                        | trial fails -> back to OPEN
//
// This is "passive" protection: it learns from REAL client connections.
// (The health checker is the "active" protection: it probes on a timer.)
//
// THREAD SAFETY: many worker threads ask "is this backend OK?" for every new
// connection, so that question must be very cheap. We answer it by reading
// std::atomic variables (no lock). The rarer changes of state (open/close) are
// done while holding a std::mutex so two threads cannot make conflicting
// decisions at the same instant.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class CircuitBreaker {
 public:
  enum class State { Closed, Open, HalfOpen };

  CircuitBreaker(std::string owner_name, int failure_threshold, int cooldown_ms);

  // Cheap, lock-free, NO side effects. "Would I be willing to send traffic here?"
  // Used by the balancers while they are just *looking* for a candidate.
  bool is_available() const;

  // Called once the balancer has CHOSEN this backend. In the Open state, once the
  // cooldown is over, exactly one caller wins the right to be the trial request
  // (state becomes HalfOpen). Returns false if the caller lost the race.
  bool try_acquire();

  void on_success();  // a connection to the backend worked
  void on_failure();  // a connection to the backend failed
  void reset();       // force back to Closed (used when a health check recovers)

  State state() const { return state_.load(std::memory_order_acquire); }
  static const char* state_name(State s);

 private:
  const std::string owner_;
  const int threshold_;
  const int cooldown_ms_;

  std::atomic<State> state_{State::Closed};
  std::atomic<int64_t> retry_at_ms_{0};  // when Open/HalfOpen may allow a trial

  mutable std::mutex mu_;           // protects the transitions below
  int consecutive_failures_ = 0;    // only touched while holding mu_
};
