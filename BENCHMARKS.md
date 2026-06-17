# Benchmark Methodology

No headline throughput or latency numbers are published yet. This file documents the harness, current limitations, and the rules for adding results without overclaiming.

## Why Open Loop

The original `scripts/benchmark.py` is a closed-loop smoke benchmark: a client sends a request, waits for the echo, then sends the next request. That is useful for correctness, but it hides queueing delay under overload.

Use `scripts/open_loop_benchmark.py` for any published latency number. It schedules arrivals at a fixed target QPS and measures scheduled-send to full-response latency, which makes overload visible in p99 and p99.9.

The harness intentionally measures from the scheduled send time, not from the moment a coroutine happens to run. That avoids the most obvious coordinated-omission failure mode in the closed-loop smoke benchmark.

Current limitation: the Python harness uses an in-memory sorted list, not a true HdrHistogram implementation. Treat it as acceptable for local development sweeps and regression detection, but do not present the results as final benchmark-grade latency data until the harness records into HdrHistogram-compatible buckets and exports corrected percentiles.

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

## Local Sweep

Use a sweep to compare changes across connection counts and payload sizes before picking a single demo number:

```bash
for conns in 1000 5000 10000; do
  for payload in 64 128 1024; do
    python3 scripts/open_loop_benchmark.py \
      --host 127.0.0.1 \
      --port 9000 \
      --connections "$conns" \
      --qps 20000 \
      --duration-seconds 30 \
      --payload-bytes "$payload" \
      --csv "results-${conns}-${payload}.csv"
  done
done
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
- Whether percentiles came from the current sorted-list harness or a future HdrHistogram-compatible recorder
- Whether the dashboard/admin server was enabled during the run
- Backend health-check interval and scheduler policy

## Publication Rules

- README numbers must come from `scripts/open_loop_benchmark.py` or a stricter replacement, never `scripts/benchmark.py`.
- Include the command line, hardware metadata, and commit SHA next to every published result.
- Do not mix smoke-test latency and open-loop latency in the same table.
- If the harness falls behind the requested QPS, publish achieved QPS and target QPS together.
- Keep raw CSV output under an ignored results directory or attach it to a release/PR rather than committing large generated files.

## Current Status

The repository includes a smoke benchmark, an open-loop scheduled-arrival harness, and CI coverage for the Linux build. No hardware-specific performance claims are currently made in the README.
