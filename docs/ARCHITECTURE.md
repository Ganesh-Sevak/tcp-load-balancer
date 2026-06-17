# Architecture

This project is a TCP proxy built around a shared-nothing worker model. The goal is to keep the hot path simple, nonblocking, and easy to reason about under load.

## Runtime Model

```text
clients
   |
   v
SO_REUSEPORT listener sockets
   |
   +--> worker 0: epoll + accepted connection pairs + per-worker counters
   +--> worker 1: epoll + accepted connection pairs + per-worker counters
   +--> worker N: epoll + accepted connection pairs + per-worker counters
   |
   v
backend pool + scheduler + health state
```

Each worker owns:

- One listener socket bound with `SO_REUSEPORT`
- One `epoll` instance
- All client/backend socket pairs accepted by that worker
- Reusable read buffers and per-worker metric counters

Shared state is limited to backend scheduler state, backend health, and atomics aggregated by the admin endpoint. The proxy data path does not perform blocking admin work.

## Connection Lifecycle

1. A worker accepts a client connection.
2. The scheduler selects a routable backend using the configured policy.
3. The worker starts a nonblocking backend connection and tracks a connect deadline.
4. Once connected, bytes read from one socket are appended to the paired socket's output buffer.
5. If an output buffer crosses the high-water mark, reads from the source socket are paused until the buffer drains.
6. Connect timeout, idle timeout, socket error, EOF, or backend failure closes only that connection pair.

## Buffering Strategy

`lb::OutputBuffer` owns queued bytes for one socket direction. It tracks a read offset instead of erasing from the front on every partial write, so common short writes are cheap. The buffer compacts only after the consumed prefix is both large and at least half of retained storage, which keeps memory bounded without adding frequent memmoves to the write path.

The event loop uses `readable_span()` for contiguous writes and `consume()` after successful writes. Back-pressure decisions use the unread byte count, not retained capacity.

## Scheduling

Supported policies:

- `round-robin`: atomic rotating index, skipping unroutable backends
- `least-connections`: selects the routable backend with the lowest active count
- `power-of-two-choices`: samples two routable candidates and selects the lower active count

Backends can be `UP`, `DOWN`, or `DRAINING`. New traffic is routed only to `UP` backends.

## Observability Contract

The admin server exposes:

- `GET /stats`: JSON snapshot consumed by the dashboard
- `GET /metrics`: Prometheus text exposition
- `GET /events`: Server-Sent Events stream of `/stats`
- `POST /backends/{id}/drain`: remove a backend from new routing
- `POST /backends/{id}/enable`: return a backend to active routing

Metrics are written as per-worker atomics on the data path and aggregated on read by the admin server. Backend connect latency is tracked with fixed buckets and exported to both JSON and Prometheus.

## Current Constraints

- The network server target is Linux-only because it uses `epoll`, `timerfd`, and `SO_REUSEPORT`.
- macOS builds still compile `lb_core` and run portable tests.
- Published performance numbers must come from the open-loop benchmark harness documented in `BENCHMARKS.md`.
