# Performance Notes

This document tracks implementation choices that keep the proxy data path predictable.

## Hot Path Principles

- Keep socket I/O nonblocking and event-loop local.
- Avoid control-plane work in worker loops.
- Store per-worker counters and aggregate on admin reads.
- Bound connection memory with high-water marks and compacting output buffers.
- Treat per-connection errors as pair-local failures, not worker failures.

## Output Buffer

`lb::OutputBuffer` uses a vector plus read offset rather than erasing on every partial write. This gives contiguous writes to `write(2)` while avoiding repeated front-removal memmoves.

Compaction happens only when:

- the consumed prefix exceeds the configured threshold, and
- the consumed prefix is at least half of retained storage.

That policy keeps retained memory bounded for long-lived partially written streams while keeping the common write/consume path cheap.

## Metrics

Proxy workers write to cache-line-aligned per-worker counters. `/stats` and `/metrics` aggregate those counters on read. This keeps observability from adding a single global counter bottleneck to every data-path event.

Backend connect latency is recorded into fixed buckets. The dashboard and Prometheus exporter read percentiles from bucket boundaries rather than storing unbounded samples.

## Parsing

Config numeric fields use `std::from_chars`, which is locale-independent, non-allocating, and validates the full input string. This rejects partial values such as `9000abc` instead of silently accepting prefixes.

## Current Follow-Ups

- Linux benchmark run with the open-loop harness on documented hardware
- `perf`/flamegraph pass on worker hot paths
- Optional edge-triggered `epoll` mode after benchmark coverage is in place
- More scheduler policies with explicit tests and config examples
