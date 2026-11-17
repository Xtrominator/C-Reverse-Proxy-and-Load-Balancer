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



cs endpoint.
