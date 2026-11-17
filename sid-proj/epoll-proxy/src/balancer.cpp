#include "balancer.hpp"

#include <algorithm>
#include <atomic>

#include "md5.hpp"

// ---------------------------------------------------------------------------
// Balancer::acquire  --  shared by all strategies
// ---------------------------------------------------------------------------
Backend* Balancer::acquire(const std::string& client_key, uint64_t exclude_mask) {
  // Two worker threads might pick the same backend in the very same instant, and
  // only one may become the circuit breaker's "half-open trial". The loser
  // excludes that backend and picks again. The loop is bounded by backend count.
  for (size_t attempt = 0; attempt < backends_.size(); ++attempt) {
    Backend* b = pick(client_key, exclude_mask);
    if (b == nullptr) return nullptr;
    if (b->breaker.try_acquire()) {
      b->active_conns.fetch_add(1, std::memory_order_relaxed);
      return b;
    }
    exclude_mask |= (1ULL << b->index);
  }
  return nullptr;
}

namespace {

// ---------------------------------------------------------------------------
// Round robin
// ---------------------------------------------------------------------------
class RoundRobin : public Balancer {
 public:
  using Balancer::Balancer;  // reuse the base constructor
  const char* name() const override { return "round-robin"; }

 protected:
  Backend* pick(const std::string&, uint64_t mask) override {
    const size_t n = backends_.size();
    // fetch_add is an ATOMIC "read the counter and add 1": two threads can never
    // receive the same number. That is the entire thread-safety story here.
    const size_t start = counter_.fetch_add(1, std::memory_order_relaxed) % n;
    for (size_t i = 0; i < n; ++i) {
      Backend* b = backends_[(start + i) % n];  // walk forward, skipping bad ones
      if (usable(b, mask)) return b;
    }
    return nullptr;
  }

 private:
  std::atomic<size_t> counter_{0};
};

// ---------------------------------------------------------------------------
// Least connections
// ---------------------------------------------------------------------------
class LeastConnections : public Balancer {
 public:
  using Balancer::Balancer;
  const char* name() const override { return "least-connections"; }

 protected:
  Backend* pick(const std::string&, uint64_t mask) override {
    const size_t n = backends_.size();
    // Scan everyone and remember the one with the smallest active_conns. We start
    // scanning at a rotating position so that when several backends tie (for
    // example all at 0 when idle) the winner rotates instead of always being #0.
    const size_t start = tiebreak_.fetch_add(1, std::memory_order_relaxed) % n;
    Backend* best = nullptr;
    int best_load = 0;
    for (size_t i = 0; i < n; ++i) {
      Backend* b = backends_[(start + i) % n];
      if (!usable(b, mask)) continue;
      int load = b->active_conns.load(std::memory_order_relaxed);
      if (best == nullptr || load < best_load) {
        best = b;
        best_load = load;
      }
    }
    return best;
    // NOTE: the numbers can change while we scan (other threads are working).
    // That is fine: "approximately the least loaded" is good enough, and avoiding
    // a global lock keeps the hot path fast.
  }

 private:
  std::atomic<size_t> tiebreak_{0};
};

// ---------------------------------------------------------------------------
// Ketama consistent hashing
// ---------------------------------------------------------------------------
// Picture a clock face with numbers 0 .. 2^32-1 laid around a circle ("the ring").
// Each backend is dropped onto the ring at 160 pseudo-random spots ("points").
// To route a client we hash its IP to a spot on the circle and walk clockwise
// to the first point we meet; that point's backend gets the client.
//
// Why 160 points per backend instead of 1? With one point each, the arcs between
// points would be wildly uneven. 160 random points smooth the load out.
//
// Why is it "consistent"? If backend B disappears, only the clients whose walk
// ended on B's points continue clockwise to the next backend. Clients of A and C
// are untouched. With plain "hash % N" almost EVERY client would move.
struct RingPoint {
  uint32_t point;     // position on the circle
  int backend_index;  // which backend owns this position
};

// Turn 4 bytes of an MD5 digest (starting at 4*h) into one 32-bit number.
// This is the exact recipe used by the original libketama library.
uint32_t digest_to_point(const std::array<uint8_t, 16>& d, int h) {
  return (static_cast<uint32_t>(d[3 + h * 4]) << 24) | (static_cast<uint32_t>(d[2 + h * 4]) << 16) |
         (static_cast<uint32_t>(d[1 + h * 4]) << 8) | static_cast<uint32_t>(d[h * 4]);
}

class Ketama : public Balancer {
 public:
  explicit Ketama(std::vector<Backend*> backends) : Balancer(std::move(backends)) {
    // 40 hashes x 4 points per hash = 160 points per backend (libketama's default).
    for (Backend* b : backends_) {
      for (int i = 0; i < 40; ++i) {
        auto digest = md5(b->name + "-" + std::to_string(i));
        for (int h = 0; h < 4; ++h) ring_.push_back({digest_to_point(digest, h), b->index});
      }
    }
    // Sort by position so we can binary-search the ring. Ties are broken by
    // backend index so the order is identical on every run.
    std::sort(ring_.begin(), ring_.end(), [](const RingPoint& a, const RingPoint& b) {
      return a.point != b.point ? a.point < b.point : a.backend_index < b.backend_index;
    });
    // The ring is never modified after this constructor ends. Many threads may
    // read it simultaneously with no lock because nobody writes to it.
  }

  const char* name() const override { return "ketama (consistent hashing)"; }

 protected:
  Backend* pick(const std::string& key, uint64_t mask) override {
    if (ring_.empty()) return nullptr;
    const uint32_t h = ketama_hash(key);

    // Binary search: first ring point whose position is >= h.
    auto it = std::lower_bound(ring_.begin(), ring_.end(), h,
                               [](const RingPoint& p, uint32_t value) { return p.point < value; });
    size_t start = static_cast<size_t>(it - ring_.begin());  // == size() means "wrap to 0"

    // Walk clockwise until we find a backend that is allowed. When the "owner"
    // is down, this naturally falls through to the next backend on the ring.
    for (size_t step = 0; step < ring_.size(); ++step) {
      const RingPoint& p = ring_[(start + step) % ring_.size()];
      Backend* b = backends_[p.backend_index];
      if (usable(b, mask)) return b;
    }
    return nullptr;
  }

 private:
  std::vector<RingPoint> ring_;
};

}  // namespace

uint32_t ketama_hash(const std::string& key) { return digest_to_point(md5(key), 0); }

std::unique_ptr<Balancer> make_balancer(const std::string& algo, std::vector<Backend*> backends) {
  if (algo == "rr") return std::make_unique<RoundRobin>(std::move(backends));
  if (algo == "lc") return std::make_unique<LeastConnections>(std::move(backends));
  if (algo == "ketama") return std::make_unique<Ketama>(std::move(backends));
  return nullptr;
}
