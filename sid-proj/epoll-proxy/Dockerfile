# ---------------------------------------------------------------------------
# Multi-stage Docker build.
#   Stage 1 ("build"): a big image with compiler + cmake, produces the binary.
#   Stage 2 (final) : a small image that contains ONLY the binary.
# Result: a tiny runtime image, because the compiler is thrown away.
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt .
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)"

FROM debian:bookworm-slim
COPY --from=build /src/build/proxy /usr/local/bin/proxy
EXPOSE 8080
# Run as a normal user: the proxy needs no special privileges (port 8080 > 1024).
RUN useradd --system --no-create-home proxy
USER proxy
ENTRYPOINT ["proxy"]
# Default arguments; docker-compose.yml overrides them.
CMD ["--port", "8080", "--backend", "127.0.0.1:9001"]
