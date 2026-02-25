# ── Stage 1: Build ────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    g++ \
    make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build -DBUILD_TESTS=OFF \
    && cmake --build build --parallel

# ── Stage 2: Runtime ──────────────────────────────────────────────────────────
FROM ubuntu:24.04

WORKDIR /app
COPY --from=builder /app/build/bin/tcp_server .

EXPOSE 8080
ENTRYPOINT ["./tcp_server"]
CMD ["8080"]
