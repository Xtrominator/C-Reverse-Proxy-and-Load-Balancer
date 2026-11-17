#!/bin/bash
# Compare round-robin and least-connections when ONE backend is overloaded.
# Requires: wrk (apt install wrk), python3. Run after building:   bash tools/bench.sh
#
# Setup: two fast backends and one "weak" backend with low capacity
# (25 ms per request, 4 at a time => about 160 requests/s).
# We send "Connection: close" requests so EVERY request is a new connection and
# the balancer gets to decide every time. (With long-lived keep-alive connections
# the choice is made once per connection, so least-connections cannot help.)
cd "$(dirname "$0")/.."
ulimit -n 65535 2>/dev/null
cleanup() { kill $(jobs -p) 2>/dev/null; }
trap cleanup EXIT

python3 tools/backend.py --port 9001 --name fast1 --delay-ms 2  --concurrency 8 >/dev/null &
python3 tools/backend.py --port 9002 --name fast2 --delay-ms 2  --concurrency 8 >/dev/null &
python3 tools/backend.py --port 9003 --name weak  --delay-ms 25 --concurrency 4 >/dev/null &
sleep 1

for algo in rr lc; do
  ./build/proxy --port 8080 --algo $algo \
     --backend 127.0.0.1:9001 --backend 127.0.0.1:9002 --backend 127.0.0.1:9003 2>/dev/null &
  PROXY_PID=$!
  sleep 0.5
  echo "=== $algo ==="
  wrk -t2 -c100 -d15s --latency -H "Connection: close" http://127.0.0.1:8080/ | grep -E "50%|99%|Requests/sec"
  kill $PROXY_PID; wait $PROXY_PID 2>/dev/null
done

# For the "10,000 concurrent connections" test (needs a big `ulimit -n` and, ideally,
# fast backends, a multi-core machine, and wrk running on a DIFFERENT machine):
#   ./build/proxy --backend ... &
#   wrk -t4 -c10000 -d30s --latency http://127.0.0.1:8080/
