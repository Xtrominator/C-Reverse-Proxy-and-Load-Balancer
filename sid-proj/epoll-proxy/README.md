# epoll-proxy: an event-driven TCP reverse proxy and load balancer in C++17

A from-scratch reverse proxy for Linux: non-blocking sockets, one `epoll` loop per
worker thread, three load-balancing algorithms (round-robin, least-connections,
Ketama consistent hashing), active health checks and a circuit breaker.
Every source file starts with a plain-English explanation and is commented line by line.

```
                      +---------------------------- proxy process -----------------------------+
                      |                                                                         |
  clients  ---------> |  worker 1: epoll loop --+                       +--> backend A (:9001)  |
 (browsers, wrk)      |  worker 2: epoll loop --+--> Balancer --------->+--> backend B (:9002)  |
                      |  worker 3: epoll loop --+   rr | lc | ketama    +--> backend C (:9003)  |
                      |       ^                          ^                         ^            |
                      |       |   (kernel spreads        | reads                   |            |
                      |       |    new connections       | healthy / breaker       | probes     |
                      |       |    via SO_REUSEPORT)     +----- shared atomics ----+            |
                      |                                  health-check threads (1 per backend)   |
                      +-------------------------------------------------------------------------+
```

## Build, test, run

```bash
# 1. build  (needs g++ 8+ and cmake 3.10+:  sudo apt install g++ cmake)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 2. unit checks (no network needed): MD5 vectors, Ketama, round-robin, least-conn, breaker
./build/proxy --self-test

# 3. try it by hand: three fake backends + the proxy
python3 tools/backend.py --port 9001 --name A &
python3 tools/backend.py --port 9002 --name B &
python3 tools/backend.py --port 9003 --name C &
./build/proxy --port 8080 --algo rr --stats \
    --backend 127.0.0.1:9001 --backend 127.0.0.1:9002 --backend 127.0.0.1:9003
curl localhost:8080          # run it several times; kill a backend; keep curling

# 4. scripted failover demo / benchmark
bash tools/demo.sh
bash tools/bench.sh          # needs wrk:  sudo apt install wrk

# 5. Docker
docker compose up --build
```

`./build/proxy --help` lists every option.

## Reading order (suggested)

| # | File | What you learn |
|---|------|----------------|
| 1 | `src/config.hpp` | every setting in one place |
| 2 | `src/util.*` | clock, `TCP_NODELAY`, thread-safe logging |
| 3 | `src/backend.hpp` | what a backend is; why `std::atomic` |
| 4 | `src/circuit_breaker.*` | closed / open / half-open state machine; atomics + mutex together |
| 5 | `src/md5.*` | the hash Ketama needs (skim it, it is just maths) |
| 6 | `src/balancer.*` | round-robin, least-connections, Ketama ring |
| 7 | `src/health_checker.*` | probing with a non-blocking connect and `poll()` |
| 8 | `src/connection.hpp` | the data structure for one client<->backend pair |
| 9 | `src/worker.*` | **the heart:** the epoll event loop |
| 10 | `src/main.cpp` | wiring, signals, `--self-test` |

## The life of one connection (worker.cpp)

1. `epoll_wait` reports the **listening socket** is readable, so `accept_ready()` calls `accept4()`.
2. A `Connection` object is created and `begin_connect()` asks the **balancer** for a backend.
3. A non-blocking `connect()` starts and returns `EINPROGRESS`; we watch the backend socket for `EPOLLOUT`.
   Meanwhile any bytes the client sends are read into the `c2b` buffer.
4. `EPOLLOUT` fires: `on_connect_event()` checks `SO_ERROR`.
   * **Failure**: the circuit breaker is told, this backend is excluded for this client, and we
     retry on another backend. The client never notices, and its buffered bytes are not lost.
   * **Success**: the buffered bytes are sent to the backend.
5. From then on: readable -> `recv()` into a buffer -> `send()` to the other side. If `send()` can't
   take everything (`EAGAIN`) we subscribe to `EPOLLOUT` and finish later. If a buffer
   reaches 64 KB we **stop reading** from its source (backpressure).
6. EOF handling: client closes its sending side -> `shutdown(SHUT_WR)` towards the backend (half-close).
   Backend closes and everything has been delivered -> close both sockets.
7. A timer sweep (once per second) closes connections that are idle too long or stuck connecting.

## Concepts that confuse beginners

* **Non-blocking socket**: `recv/send/connect` return immediately. "Nothing right now" is reported as
  `EAGAIN`/`EINPROGRESS`, which is *not* an error.
* **epoll, level-triggered**: "keep reporting this socket while the condition holds". We therefore
  only subscribe to events we can act on right now (`update_interest`), otherwise the loop would spin.
* **SO_REUSEPORT**: every worker binds its own listening socket to the same port and the kernel
  distributes new connections among them: no shared accept queue, no lock.
* **The graveyard**: if a connection closes in the middle of an epoll batch, later events in that batch
  may still point at it. Closed connections are only `delete`d after the batch finishes.
* **Atomics vs mutex**: hot-path counters (`active_conns`, `healthy`, round-robin counter) are
  `std::atomic`. The breaker's *state transitions* (rare, multi-step) take a `std::mutex`.
  The Ketama ring is never modified after startup, so it needs no lock at all.

## Mapping to the claims on the resume

| Claim | Where in the code |
|---|---|
| non-blocking sockets + epoll loop | `worker.cpp` (`SOCK_NONBLOCK`, `epoll_wait`, `run()`) |
| thread pool | `main.cpp` starts N `Worker` threads, each with its own epoll |
| `std::atomic` / `std::mutex` on shared routing state | `backend.hpp`, `balancer.cpp`, `circuit_breaker.cpp` |
| round-robin / least-connections / Ketama | `balancer.cpp` |
| active health checks | `health_checker.cpp` |
| circuit breaking | `circuit_breaker.cpp` |
| reroute within 3 s of a backend failure | health: 2 failed probes x 500 ms = about 1-2 s; breaker: 3 failed connects. Either alone is enough. Verified manually (see below). |

## What was verified (in a 1-CPU sandbox)

* `--self-test`: all 13 checks pass (MD5 official vectors, Ketama stability, breaker states, ...).
* Round-robin order, keep-alive pinning, POST bodies, 503 when all backends are down, and automatic
  recovery of a restarted backend all behave as described.
* Killing a backend mid-traffic: **zero failed requests**; the breaker opened after 3 failed connects and
  the health checker marked it DOWN about 1 s later.
* 8,000 simultaneous keep-alive connections: no errors, balanced 2666/2667/2667, no leaked file descriptors afterwards.
* Clean runs under **ThreadSanitizer** and **AddressSanitizer** (including a backend killed and restarted under load).
* Least-connections vs round-robin with one overloaded backend (`tools/bench.sh` scenario): p99 about 2.8x lower
  and throughput much higher with `lc`. Your number will differ.

**The throughput figures in this sandbox are NOT 45,000 req/s benchmarks.** One CPU core was shared by the
proxy, three Python backends and `wrk`. Do the real measurement yourself on a multi-core machine (ideally `wrk` and
the backends on separate hosts) and put **your own measured numbers** on the resume. Be ready to explain how you measured.

## Honest design limits (good interview material)

* It is a **layer-4 (TCP) proxy**: it balances *connections*, not individual HTTP requests, and does not parse HTTP.
  One keep-alive client connection stays on one backend. This is why `tools/bench.sh` uses `Connection: close`:
  least-connections can only help when new connections keep arriving.
* IPv4 only; max 64 backends (a `uint64_t` bit mask); no TLS; no config reload.
* If *every* backend is marked down the proxy returns 503 (no "panic mode").
* Possible upgrades: HTTP parsing for per-request balancing, `splice()` for zero-copy forwarding, weights,
  hot reload with a mutex-protected snapshot, a Prometheus metrics endpoint.
