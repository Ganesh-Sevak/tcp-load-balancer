# AGENTS.md — Build guidance for Codex

This file is the project brief for AI coding agents (Codex et al.) working in this
repo. The goal of this round of work is explicit:

> **Make this project genuinely impressive to FAANG and quant/HFT SWE recruiters,
> and build a real-time observability UI for it.**

Read this whole file before touching code. The changes below are ordered by
technical impact. Ship them as small, reviewable, independently-mergeable
PRs — each with tests, a benchmark or screenshot where relevant, and a README
update. Keep CI green and keep the binary building on Linux at every step.

---

## 1. What exists today (baseline)

A single-threaded C++20 TCP load balancer for Linux:

- `epoll` **level-triggered** event loop, one thread — `src/server.cpp`.
- Scheduling — `src/scheduler.cpp`: round-robin (atomic counter) and
  least-connections (global mutex + linear scan on every `select()`).
- Per-connection `Peer` with an outbound buffer; back-pressure via a 1 MiB
  high-water mark that toggles `EPOLLIN` on the source socket.
- ini-style config parser — `src/config.cpp`.
- Hand-rolled assertion tests — `tests/`; CI builds + runs `ctest` on Ubuntu.
- Python helpers — `scripts/echo_backends.py`, `scripts/benchmark.py` (closed-loop).

**It works, but it reads like a course project.** The README claims it is
"designed for 10K+ simultaneous connections," yet one core does all I/O, there is
no failover, no timeouts, and no observability. Recruiters at quant shops will
read the code and probe exactly these gaps. The work below closes them.

---

## 2. Known correctness gaps — fix these first

These are real defects. Fixing them is cheap and signals rigor.

1. **No backend connect timeout.** `connect_backend()` returns a fd in
   `EINPROGRESS` and registers it for `EPOLLOUT`. If a backend accepts the TCP
   handshake but never completes it (or is a black hole), the connection hangs
   forever and the client is stuck. Add a per-connection connect deadline using a
   `timerfd` (or a timing wheel) and tear the pair down on expiry.
2. **No idle timeout.** Connections live forever with no data. Add a configurable
   idle timeout and reap stale pairs.
3. **README overclaims concurrency.** Until item 3.1 lands, the "10K+ connections"
   line is misleading on a single thread. Either land sharded threading or soften
   the claim — don't ship a project claim the code can't back up.
4. **`std::cout`/`std::cerr` logging only.** No levels, no structure. Introduce a
   tiny leveled logger (or spdlog) with timestamps; this also feeds the metrics work.
5. **Hardcoded `raise_file_limit()` target of 25000.** Drive it from config.

---

## 3. Highest-impact technical work (ranked)

### 3.1 — Multi-threaded, sharded event loop with `SO_REUSEPORT`  ⭐ headline change
Single-threaded is the project's biggest credibility ceiling. Move to **N worker
threads, each owning its own `epoll` instance and its own listener socket opened
with `SO_REUSEPORT`**, so the kernel load-balances accepts across cores (the
nginx / Seastar-style "shared-nothing per-core" model). Each worker owns its
connections with no cross-thread sharing on the data path. This is the single
change that most elevates the project from "toy" to "systems engineer." Make worker
count configurable (default = hardware concurrency). Document the architecture in
the README with a diagram.

### 3.2 — Active health checks + automatic failover / ejection  ⭐
A load balancer that can't detect a dead backend isn't a load balancer. Add:
- Periodic active health checks (TCP-connect probe, configurable interval/timeout).
- Passive ejection: after N consecutive connection/IO failures, mark a backend
  `DOWN`; stop routing to it; re-probe and re-admit after it recovers
  (outlier-detection style).
- `Scheduler::select()` must skip `DOWN` backends and return `nullopt` only when
  all are down. Add tests for ejection and recovery.

### 3.3 — Observability: metrics + admin endpoint  ⭐ (also powers the UI)
Expose runtime state over a small **admin HTTP server on a separate port**
(e.g. `9100`), serving:
- `GET /stats` → JSON snapshot: uptime, listener, policy, worker count, total &
  active connections, bytes in/out, connections/sec, errors; and a per-backend
  array: address, health state, active conns, total conns, bytes, error count,
  EWMA/last connect latency.
- `GET /metrics` → Prometheus text exposition format (counters/gauges/histograms).
- `GET /events` (Server-Sent Events) or a WebSocket → pushes the stats snapshot
  ~2–4x/sec for the live UI. (SSE is simpler in C++ and sufficient.)
- `POST /backends/{id}/drain` and `/enable` → mark a backend draining/active.

Implement the metrics counters as per-worker atomics aggregated on read to avoid
contention on the hot path. **This endpoint is the contract the UI consumes — build
it before the UI.**

### 3.4 — Rigorous, open-loop benchmarking with latency histograms  ⭐ quant catnip
The current `benchmark.py` is **closed-loop** (each client waits for its own echo
before sending again), so its p99 is corrupted by **coordinated omission** — a real
red flag to quant interviewers. Replace/augment it:
- Open-loop / constant-arrival-rate load generation (fixed target QPS, record
  scheduled-vs-actual send time).
- Use an **HdrHistogram** and report p50/p90/p99/p99.9/p99.99 + max, plus
  coordinated-omission-corrected percentiles.
- Sweep connection counts and payload sizes; emit CSV the UI / README can chart.
- Document the methodology and the hardware in `BENCHMARKS.md`. Show before/after
  numbers for 3.1.

### 3.5 — Smarter scheduling policies
Signals breadth of reading. Add:
- **Power-of-two-choices (P2C)**: pick 2 random backends, route to the
  lower-active one — near-optimal load spreading without a global scan.
- **Weighted round-robin** (per-backend weights in config).
- **Consistent hashing** (by client IP) for session stickiness.
Replace the least-connections global mutex + O(n) scan with per-backend atomics so
`select()` is contention-free; explain the memory ordering in comments.

### 3.6 — Edge-triggered epoll (`EPOLLET`)
Convert the loop to edge-triggered with correct full-drain read/write loops
(`EAGAIN`-terminated). The current code already drains in loops, so this is a
natural, high-signal upgrade — but it must be done carefully and validated under
load. Gate it behind the test + benchmark suite.

### 3.7 — Test & quality engineering depth
- Replace hand-rolled asserts with **GoogleTest** or **Catch2**.
- Add an **integration test**: spin up echo backends, drive traffic through the LB,
  assert byte-exact echo, then kill a backend and assert failover.
- Add CI jobs running tests under **ASan/UBSan** and **TSan** (TSan is a strong
  signal for a concurrent network server). Add `clang-format` + `clang-tidy` checks.
- Add a **libFuzzer** target for the config parser.

### 3.8 — Stretch goals (pick if time allows, each is a strong differentiator)
- **PROXY protocol v1/v2** support to preserve the real client IP to backends.
- **`io_uring`** backend as an alternative to epoll (feature-flagged) — cutting-edge.
- **Graceful connection draining** on SIGTERM (stop accepting, let in-flight finish).
- **TLS termination** in front of plaintext backends.

---

## 4. Build the UI — a real-time load-balancer control dashboard

The user explicitly wants a UI. A TCP LB has no inherent GUI, so build the thing
that is genuinely impressive and interview-worthy: a **live observability &
control dashboard** that consumes the admin endpoint from 3.3.

**Stack:** Vite + React (matches the user's other projects). Keep it a single
small app under `web/` (or `dashboard/`). Plain modern CSS or a light component
lib; no heavyweight framework. Add an npm `dev`/`build` and wire it into the
README + Docker (serve the built static assets from the admin HTTP server, or via
a separate nginx container in docker-compose).

**Data flow:** the dashboard subscribes to `GET /events` (SSE) for ~2–4 Hz live
updates and falls back to polling `GET /stats`. **Include a mock-data mode** so the
UI renders and animates with no backend running (great for demos and screenshots).

**The dashboard must show:**
1. **Header status** — listener address, active policy, worker count, uptime,
   connection to admin endpoint (live / mock / offline).
2. **Throughput panel** — live line chart of req/s and bytes/s (in & out), with a
   rolling window.
3. **Active-connections gauge** — current vs peak.
4. **Latency panel** — p50 / p99 / p99.9 read from the histogram, ideally a small
   histogram/heatmap visualization.
5. **Backend table** — one row per backend: address, health badge
   (UP / DOWN / DRAINING), active conns, total conns, bytes proxied, error count,
   EWMA connect latency. Color-code health.
6. **Controls** — per-backend **Drain** / **Enable** buttons (POST to admin
   endpoint), with optimistic UI + confirmation.
7. **Policy switcher** (optional) if a runtime policy-change endpoint is added.

**Quality bar:** responsive layout, dark theme, smooth chart animation, accessible
controls, and a clean empty/mock state. Add screenshots to the README — recruiters
look at the README first, and a polished dashboard over a low-level C++ engine is a
memorable combination.

---

## 5. Guardrails (do not violate)

1. **Keep CI green and the Linux binary building at every commit.** The server
   target is Linux-only (`epoll`); `lb_core` + tests must keep building on macOS.
2. **Never break the data path for a feature.** Metrics/health/UI must not add
   blocking calls, locks, or allocations on the per-packet hot path. Use per-worker
   atomics aggregated on read.
3. **The event loop must not die on a single bad connection.** All per-connection
   errors tear down only that pair, never the worker.
4. **No coordinated-omission'd latency numbers in the README.** Any published
   percentile must come from the open-loop + HdrHistogram harness (3.4).
5. **Don't overclaim.** A README or project claim must be backed by code that
   is merged and by a benchmark that was actually run on stated hardware.
6. **No secrets in the repo.** Config/flags only.
7. **Keep PRs small and scoped.** One capability per PR with its own tests.

---

## 6. Suggested delivery order

1. Correctness fixes (§2: connect timeout, idle timeout, logger).
2. Metrics + admin/stats/SSE endpoint (§3.3) — unblocks the UI.
3. Health checks + failover (§3.2).
4. Sharded multi-threaded loop with `SO_REUSEPORT` (§3.1).
5. Open-loop + HdrHistogram benchmark; publish before/after (§3.4).
6. The React dashboard (§4).
7. P2C / weighted / consistent-hash policies (§3.5), edge-triggered epoll (§3.6).
8. Test depth: GoogleTest/Catch2, integration test, TSan/ASan CI, fuzzing (§3.7).
9. Stretch goals (§3.8) as time allows.

After each merged item, update the README (and `BENCHMARKS.md`) so the project's
story stays accurate and compelling.
