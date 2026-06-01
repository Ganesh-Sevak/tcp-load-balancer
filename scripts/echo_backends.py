#!/usr/bin/env python3
import argparse
import asyncio


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


async def main():
    parser = argparse.ArgumentParser(description="Start TCP echo backends for local load balancer testing")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--ports", type=int, nargs="+", default=[9101, 9102])
    args = parser.parse_args()

    servers = [await echo_server(args.host, port) for port in args.ports]
    print("echo backends listening on " + ", ".join(f"{args.host}:{port}" for port in args.ports))

    try:
        await asyncio.Event().wait()
    finally:
        for server in servers:
            server.close()
            await server.wait_closed()


if __name__ == "__main__":
    asyncio.run(main())

