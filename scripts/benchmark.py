#!/usr/bin/env python3
import argparse
import asyncio
import statistics
import time


async def echo_server(host: str, port: int):
    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        try:
            while data := await reader.read(65536):
                writer.write(data)
                await writer.drain()
        finally:
            writer.close()
            await writer.wait_closed()

    return await asyncio.start_server(handle, host, port, reuse_address=True)


async def client(host: str, port: int, requests: int, payload: bytes, latencies: list[float]):
    reader, writer = await asyncio.open_connection(host, port)
    try:
        for _ in range(requests):
            start = time.perf_counter()
            writer.write(payload)
            await writer.drain()
            remaining = len(payload)
            while remaining:
                chunk = await reader.read(remaining)
                if not chunk:
                    raise ConnectionError("connection closed before full echo response")
                remaining -= len(chunk)
            latencies.append((time.perf_counter() - start) * 1000)
    finally:
        writer.close()
        await writer.wait_closed()


async def run(args: argparse.Namespace):
    servers = []
    if args.start_echo_backends:
        for port in args.echo_ports:
            servers.append(await echo_server("127.0.0.1", port))
        print(f"started echo backends on {', '.join(map(str, args.echo_ports))}")

    latencies: list[float] = []
    payload = b"x" * args.payload_bytes
    start = time.perf_counter()

    tasks = [
        asyncio.create_task(client(args.host, args.port, args.requests_per_client, payload, latencies))
        for _ in range(args.connections)
    ]
    await asyncio.gather(*tasks)

    elapsed = time.perf_counter() - start
    total_requests = args.connections * args.requests_per_client
    sorted_latencies = sorted(latencies)
    p99_index = max(0, int(len(sorted_latencies) * 0.99) - 1)

    print(f"connections: {args.connections}")
    print(f"requests: {total_requests}")
    print(f"throughput: {total_requests / elapsed:.2f} req/s")
    print(f"median latency: {statistics.median(sorted_latencies):.3f} ms")
    print(f"p99 latency: {sorted_latencies[p99_index]:.3f} ms")

    for server in servers:
        server.close()
        await server.wait_closed()


def parse_args():
    parser = argparse.ArgumentParser(description="TCP echo benchmark for tcp-load-balancer")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--connections", type=int, default=1000)
    parser.add_argument("--requests-per-client", type=int, default=10)
    parser.add_argument("--payload-bytes", type=int, default=128)
    parser.add_argument("--start-echo-backends", action="store_true")
    parser.add_argument("--echo-ports", type=int, nargs="+", default=[9101, 9102])
    return parser.parse_args()


if __name__ == "__main__":
    asyncio.run(run(parse_args()))

