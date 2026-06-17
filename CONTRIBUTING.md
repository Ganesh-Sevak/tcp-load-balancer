# Contributing

This repository is intentionally structured as a systems project with small, reviewable changes. Keep each PR focused on one capability: data path, scheduler, observability, dashboard, benchmark, or CI.

## Local Checks

Run these before opening a PR:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
npm --prefix web run build
```

The server binary is Linux-only. On macOS, CMake builds portable core code and tests but skips the `tcp-load-balancer` executable.

## Pull Request Expectations

- Keep the README accurate. Do not add performance claims without benchmark data from `BENCHMARKS.md`.
- Add or update tests for scheduler, runtime, config, admin route, or dashboard behavior when those contracts change.
- Avoid blocking calls, locks, or heap churn in the proxy data path.
- Treat backend/admin/dashboard features as separate PRs when possible.
- Include screenshots for dashboard-facing changes.

## Benchmark Claims

Use `scripts/open_loop_benchmark.py` for serious latency claims. Do not publish numbers from the closed-loop smoke benchmark as headline results.

Every published result should include hardware, kernel, commit SHA, worker count, backend count, target QPS, achieved QPS, payload size, and percentile table.
