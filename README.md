# TCP Load Balancer

A high-performance TCP load balancer written in modern C++ for Linux. It proxies raw TCP streams to backend services using nonblocking sockets and `epoll`, with round-robin and least-connections scheduling policies.

## Features

- Nonblocking `epoll` event loop for high connection concurrency
- Round-robin and least-connections backend scheduling
- Per-connection buffering with read throttling for back-pressure
- Config-file driven listener, backend, and policy selection
- Unit-tested scheduler logic
- Docker build for Linux deployment
- Async benchmark helper for throughput and p99 latency checks

## Project Layout

```text
.
├── CMakeLists.txt
├── Dockerfile
├── config/backends.conf
├── include/lb
├── scripts
│   ├── benchmark.py
│   └── run_local.sh
├── src
└── tests
```

## Configuration

Edit `config/backends.conf`:

```ini
listen=0.0.0.0:9000
policy=least-connections

backend=127.0.0.1:9101
backend=127.0.0.1:9102
```

Supported policies:

- `round-robin`
- `least-connections`

## Build

The load balancer target uses Linux `epoll`. Build it on Linux or in Docker.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On macOS, CMake still builds and runs the scheduler tests, but it skips the Linux-only server binary.

## Run

Start two local echo backends in one terminal:

```bash
python3 scripts/echo_backends.py --ports 9101 9102
```

Start the load balancer in another terminal on Linux:

```bash
./build/tcp-load-balancer --config config/backends.conf
```

Or run with Docker:

```bash
docker build -t tcp-load-balancer .
docker run --network host tcp-load-balancer
```

## Benchmark

With echo backends and the load balancer running:

```bash
python3 scripts/benchmark.py \
  --host 127.0.0.1 \
  --port 9000 \
  --connections 10000 \
  --requests-per-client 10 \
  --payload-bytes 128
```

The benchmark reports total request throughput, median latency, and p99 latency. For 10K simultaneous connections, raise the OS file descriptor limit first:

```bash
ulimit -n 25000
```

## Design Notes

Each client connection is paired with a backend connection. Incoming data is appended to the paired socket's output buffer and flushed whenever the socket is writable. If a paired output buffer grows beyond the high-water mark, reads from the source socket are temporarily disabled, applying back-pressure instead of allowing unbounded memory growth.

Least-connections scheduling tracks active proxied connections per backend and routes new clients to the backend with the lowest current count. Round-robin uses an atomic counter to rotate across backend indexes.

## Resume Bullets

- Implemented a high-performance TCP load balancer in C++ supporting round-robin and least-connections scheduling.
- Built a concurrent connection handling model designed for 10K+ simultaneous TCP connections, with benchmark tooling for throughput and p99 latency validation under load.
