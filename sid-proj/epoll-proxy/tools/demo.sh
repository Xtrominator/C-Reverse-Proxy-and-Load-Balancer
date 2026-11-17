#!/bin/bash
# Failover demo: needs python3 + curl. Run from the project root after building:
#     bash tools/demo.sh
cd "$(dirname "$0")/.."
PROXY=./build/proxy
[ -x "$PROXY" ] || { echo "build first: cmake -S . -B build && cmake --build build"; exit 1; }

cleanup() { kill $(jobs -p) 2>/dev/null; }
trap cleanup EXIT

for p in 9001 9002 9003; do python3 tools/backend.py --port $p --name B$p >/dev/null & done
sleep 0.7
$PROXY --port 8080 --threads 2 --algo rr \
  --backend 127.0.0.1:9001 --backend 127.0.0.1:9002 --backend 127.0.0.1:9003 2>proxy.log &
sleep 0.7

echo "--- 6 requests, round-robin: expect B9001 B9002 B9003 B9001 B9002 B9003"
for i in 1 2 3 4 5 6; do curl -s localhost:8080/; done

echo; echo "--- killing backend B9002 ..."
kill $(pgrep -f "backend.py --port 9002")
sleep 0.2
echo "--- 9 more requests: every one must still succeed (no B9002)"
for i in $(seq 1 9); do curl -s localhost:8080/; done | sort | uniq -c

sleep 2
echo; echo "--- what the proxy noticed (proxy.log):"
grep -E "health|breaker" proxy.log
