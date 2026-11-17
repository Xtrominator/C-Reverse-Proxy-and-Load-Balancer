#!/usr/bin/env python3
"""
A tiny HTTP backend used to test the proxy.  No dependencies (standard library only).

    python3 tools/backend.py --port 9001 --name A
    python3 tools/backend.py --port 9002 --name B --delay-ms 20     # a SLOW backend

  GET /health   -> "200 ok" immediately (never delayed) -- the proxy's health probe
  GET anything  -> "hello from <name>" after --delay-ms milliseconds

  --concurrency N   serve at most N requests at the same time (others queue up).
                    With --delay-ms D this gives the backend a hard capacity of
                    about N / D requests per second. When more load arrives than
                    that, its queue (and latency) grows. This is how REAL servers
                    behave under overload, and it is the situation where
                    least-connections beats round-robin.
"""
import argparse
import asyncio


async def handle(reader, writer, args, sem):
    try:
        while True:
            head = await reader.readuntil(b"\r\n\r\n")          # read the request headers
            lines = head.decode("latin-1").split("\r\n")
            method, path, version = (lines[0].split(" ") + ["", "", ""])[:3]
            headers = {}
            for ln in lines[1:]:
                if ":" in ln:
                    k, v = ln.split(":", 1)
                    headers[k.strip().lower()] = v.strip()
            if "content-length" in headers:                       # swallow a request body, if any
                await reader.readexactly(int(headers["content-length"]))

            if path == "/health":
                body = b"ok\n"
            else:
                async with sem:                                   # wait for a free "worker slot"
                    if args.delay_ms:
                        await asyncio.sleep(args.delay_ms / 1000.0)   # simulate work
                body = f"hello from {args.name}\n".encode()

            close = headers.get("connection", "").lower() == "close" or version == "HTTP/1.0"
            resp = (
                "HTTP/1.1 200 OK\r\n"
                f"Content-Length: {len(body)}\r\n"
                f"X-Backend: {args.name}\r\n"
                f"Connection: {'close' if close else 'keep-alive'}\r\n\r\n"
            ).encode() + body
            writer.write(resp)
            await writer.drain()
            if close:
                break
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        writer.close()


async def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--name", default="backend")
    p.add_argument("--delay-ms", type=int, default=0)
    p.add_argument("--concurrency", type=int, default=0, help="0 = unlimited")
    args = p.parse_args()
    sem = asyncio.Semaphore(args.concurrency or 10**9)
    server = await asyncio.start_server(lambda r, w: handle(r, w, args, sem), "0.0.0.0", args.port, backlog=4096)
    print(f"backend {args.name} listening on {args.port} (delay {args.delay_ms} ms, concurrency {args.concurrency or "unlimited"})", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
