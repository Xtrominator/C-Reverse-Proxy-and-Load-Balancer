// ===========================================================================
// health_checker.hpp  --  actively probe every backend on a timer
// ===========================================================================
// One background thread per backend repeats forever:
//     connect to the backend, send "GET /health", look at the reply
//     good reply -> count a success, bad/no reply -> count a failure
// After `health_fall` failures in a row the backend is marked DOWN, and the
// balancers stop choosing it. After `health_rise` successes in a row it is
// marked UP again.
//
// Detection time with the defaults: probes every 500 ms, 2 failures needed, so a
// dead backend is noticed in roughly 1-2 s (the circuit breaker, which reacts to
// real traffic, is usually even faster).
#pragma once
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "backend.hpp"
#include "config.hpp"

class HealthChecker {
 public:
  HealthChecker(std::vector<Backend*> backends, const Config& cfg);
  ~HealthChecker() { stop(); }

  void start();
  void stop();  // safe to call more than once

 private:
  void run_for(Backend* backend);                 // body of one probing thread
  bool probe(const Backend& backend) const;       // one probe; true = healthy

  std::vector<Backend*> backends_;
  const Config& cfg_;
  std::vector<std::thread> threads_;

  // Used only to let stop() wake sleeping threads immediately instead of waiting
  // for the end of their sleep.
  std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
};
