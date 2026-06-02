# Benchmark Methodology

No headline throughput or latency numbers are published yet. This file documents the harness and the rules for adding results without overclaiming.

## Why Open Loop

The original `scripts/benchmark.py` is a closed-loop smoke benchmark: a client sends a request, waits for the echo, then sends the next request. That is useful for correctness, but it hides queueing delay under overload.

Use `scripts/open_loop_benchmark.py` for any published latency number. It schedules arrivals at a fixed target QPS and measures scheduled-send to full-response latency, which makes overload visible in p99 and p99.9.

## Run

```bash
ulimit -n 25000
python3 scripts/echo_backends.py --ports 9101 9102
./build/tcp-load-balancer --config config/backends.conf

python3 scripts/open_loop_benchmark.py \
  --host 127.0.0.1 \
  --port 9000 \
  --connections 10000 \
  --qps 50000 \
  --duration-seconds 60 \
  --payload-bytes 128 \
  --csv results.csv
```

## Required Result Metadata

When adding numbers, include:

- CPU model and core count
- RAM
- Kernel version
- File descriptor limit
- Load balancer commit SHA
- Worker count
- Backend count and backend process placement
- Payload size, connection count, target QPS, duration
- p50, p90, p99, p99.9, p99.99, max

## Current Status

The repository includes the benchmark harness and CI coverage for the Linux build. No hardware-specific performance claims are currently made in the README.
