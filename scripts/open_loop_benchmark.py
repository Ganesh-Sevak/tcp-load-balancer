#!/usr/bin/env python3
import argparse
import asyncio
import csv
import math
import time


class Histogram:
    def __init__(self):
        self.values = []

    def record(self, value_ms: float):
        self.values.append(value_ms)

    def percentile(self, percentile: float) -> float:
        if not self.values:
            return 0.0
        values = sorted(self.values)
        index = min(len(values) - 1, max(0, math.ceil((percentile / 100.0) * len(values)) - 1))
        return values[index]

    def max(self) -> float:
        return max(self.values) if self.values else 0.0


async def worker(host: str, port: int, queue: asyncio.Queue, payload: bytes, histogram: Histogram):
    reader, writer = await asyncio.open_connection(host, port)
    try:
        while True:
            scheduled_at = await queue.get()
            if scheduled_at is None:
                return
            writer.write(payload)
            await writer.drain()
            remaining = len(payload)
            while remaining:
                chunk = await reader.read(remaining)
                if not chunk:
                    raise ConnectionError("connection closed before full echo response")
                remaining -= len(chunk)
            histogram.record((time.perf_counter() - scheduled_at) * 1000)
    finally:
        writer.close()
        await writer.wait_closed()


async def run(args):
    queue = asyncio.Queue(maxsize=args.qps * 2)
    histogram = Histogram()
    payload = b"x" * args.payload_bytes
    workers = [
        asyncio.create_task(worker(args.host, args.port, queue, payload, histogram))
        for _ in range(args.connections)
    ]

    interval = 1.0 / args.qps
    started = time.perf_counter()
    deadline = started + args.duration_seconds
    sent = 0
    next_send = started

    while time.perf_counter() < deadline:
        now = time.perf_counter()
        if now < next_send:
            await asyncio.sleep(next_send - now)
        scheduled_at = next_send
        await queue.put(scheduled_at)
        sent += 1
        next_send += interval

    for _ in workers:
        await queue.put(None)
    await asyncio.gather(*workers)

    elapsed = time.perf_counter() - started
    rows = {
        "connections": args.connections,
        "target_qps": args.qps,
        "sent": sent,
        "duration_seconds": round(elapsed, 3),
        "achieved_qps": round(sent / elapsed, 2),
        "p50_ms": round(histogram.percentile(50), 3),
        "p90_ms": round(histogram.percentile(90), 3),
        "p99_ms": round(histogram.percentile(99), 3),
        "p999_ms": round(histogram.percentile(99.9), 3),
        "p9999_ms": round(histogram.percentile(99.99), 3),
        "max_ms": round(histogram.max(), 3),
    }

    for key, value in rows.items():
        print(f"{key}: {value}")

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=list(rows.keys()))
            writer.writeheader()
            writer.writerow(rows)


def parse_args():
    parser = argparse.ArgumentParser(description="Open-loop TCP echo benchmark with scheduled arrivals")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--connections", type=int, default=1000)
    parser.add_argument("--qps", type=int, default=10000)
    parser.add_argument("--duration-seconds", type=int, default=30)
    parser.add_argument("--payload-bytes", type=int, default=128)
    parser.add_argument("--csv")
    return parser.parse_args()


if __name__ == "__main__":
    asyncio.run(run(parse_args()))

