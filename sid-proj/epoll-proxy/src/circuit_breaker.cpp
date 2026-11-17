#include "circuit_breaker.hpp"

#include <utility>

#include "util.hpp"

CircuitBreaker::CircuitBreaker(std::string owner_name, int failure_threshold, int cooldown_ms)
    : owner_(std::move(owner_name)), threshold_(failure_threshold), cooldown_ms_(cooldown_ms) {}

const char* CircuitBreaker::state_name(State s) {
  switch (s) {
    case State::Closed:   return "closed";
    case State::Open:     return "OPEN";
    case State::HalfOpen: return "half-open";
  }
  return "?";
}

bool CircuitBreaker::is_available() const {
  State s = state_.load(std::memory_order_acquire);
  if (s == State::Closed) return true;
  // Open or HalfOpen: only "available" once the cooldown / trial deadline passed.
  // (If a trial request vanished without reporting back, the deadline passing
  //  lets us try again, so the breaker can never get stuck forever.)
  return util::now_ms() >= retry_at_ms_.load(std::memory_order_acquire);
}

bool CircuitBreaker::try_acquire() {
  if (state_.load(std::memory_order_acquire) == State::Closed) return true;  // fast path

  std::lock_guard<std::mutex> lock(mu_);
  if (state_.load() == State::Closed) return true;       // someone closed it meanwhile
  int64_t now = util::now_ms();
  if (now < retry_at_ms_.load()) return false;           // another thread took the trial slot
  state_.store(State::HalfOpen, std::memory_order_release);
  retry_at_ms_.store(now + cooldown_ms_, std::memory_order_release);  // next trial deadline
  LOG_INFO("breaker[%s]: half-open, sending one trial connection", owner_.c_str());
  return true;
}

void CircuitBreaker::on_success() {
  std::lock_guard<std::mutex> lock(mu_);
  consecutive_failures_ = 0;
  if (state_.load() != State::Closed) {
    state_.store(State::Closed, std::memory_order_release);
    LOG_INFO("breaker[%s]: trial succeeded -> CLOSED (backend is back)", owner_.c_str());
  }
}

void CircuitBreaker::on_failure() {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t now = util::now_ms();

  if (state_.load() == State::HalfOpen) {
    // The trial connection failed: the backend is still bad. Open again.
    state_.store(State::Open, std::memory_order_release);
    retry_at_ms_.store(now + cooldown_ms_, std::memory_order_release);
    LOG_WARN("breaker[%s]: trial failed -> OPEN for %d ms", owner_.c_str(), cooldown_ms_);
    return;
  }
  if (state_.load() == State::Open) return;  // late report from an older connection

  if (++consecutive_failures_ >= threshold_) {
    state_.store(State::Open, std::memory_order_release);
    retry_at_ms_.store(now + cooldown_ms_, std::memory_order_release);
    consecutive_failures_ = 0;
    LOG_WARN("breaker[%s]: %d failures in a row -> OPEN for %d ms", owner_.c_str(),
             threshold_, cooldown_ms_);
  }
}

void CircuitBreaker::reset() {
  std::lock_guard<std::mutex> lock(mu_);
  consecutive_failures_ = 0;
  state_.store(State::Closed, std::memory_order_release);
}
