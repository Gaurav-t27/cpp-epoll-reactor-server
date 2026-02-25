#!/usr/bin/env python3
"""
Demo for cpp-epoll-reactor-server.

The server must be running before launching this script.

    ./build/bin/tcp_server        # local build
    docker compose up -d          # Docker

Usage:
    python3 demo/demo.py              # default: 127.0.0.1:8080
    python3 demo/demo.py HOST PORT    # custom host/port
"""

import socket
import sys
import threading
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

# ANSI colours
BOLD  = "\033[1m"
DIM   = "\033[2m"
GREEN = "\033[32m"
RED   = "\033[31m"
CYAN  = "\033[36m"
RESET = "\033[0m"

SEP = f"{CYAN}{'─' * 54}{RESET}"


def header(title):
    print(f"\n{SEP}")
    print(f"{BOLD}  {title}{RESET}")
    print(SEP)


def connect_and_echo(msg: bytes) -> bytes:
    with socket.create_connection((HOST, PORT), timeout=5) as s:
        s.sendall(msg)
        return s.recv(len(msg) + 128)


# ── Demo 1: Basic echo ────────────────────────────────────────────────────────

def demo_echo():
    header("1 / 3  —  Basic Echo")
    for msg in [b"hello world", b"reactor pattern", b"epoll server"]:
        response = connect_and_echo(msg)
        print(f"  {DIM}send{RESET}  {msg.decode():<20}"
              f"  {DIM}recv{RESET}  {GREEN}{response.decode()}{RESET}")


# ── Demo 2: Concurrent clients ────────────────────────────────────────────────

def demo_concurrent():
    header("2 / 3  —  10 Concurrent Clients")
    print(f"  {DIM}All 10 connections opened simultaneously{RESET}\n")

    results = {}
    lock = threading.Lock()

    def client(i):
        msg = f"client-{i:02d}".encode()
        try:
            resp = connect_and_echo(msg)
            with lock:
                results[i] = (msg.decode(), resp.decode(), True)
        except Exception as e:
            with lock:
                results[i] = (msg.decode(), str(e), False)

    threads = [threading.Thread(target=client, args=(i,)) for i in range(10)]
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - t0

    for i in range(10):
        sent, received, ok = results[i]
        tick = f"{GREEN}✓{RESET}" if ok else f"{RED}✗{RESET}"
        colour = GREEN if ok else RED
        print(f"  {tick}  {sent}  →  {colour}{received}{RESET}")

    print(f"\n  {DIM}10 clients completed in {elapsed * 1000:.1f} ms{RESET}")


# ── Demo 3: Large payload ─────────────────────────────────────────────────────

def demo_large():
    header("3 / 3  —  Large Payload  (1 MB)")
    data = b"a" * (1024 * 1024)
    print(f"  {DIM}Sending {len(data):,} bytes...{RESET}")

    t0 = time.perf_counter()
    with socket.create_connection((HOST, PORT), timeout=15) as s:
        s.sendall(data)
        received = b""
        while len(received) < len(data):
            chunk = s.recv(65536)
            if not chunk:
                break
            received += chunk
    elapsed = time.perf_counter() - t0

    ok = received == data.upper()
    tick = f"{GREEN}✓ correct{RESET}" if ok else f"{RED}✗ mismatch{RESET}"
    mbps = (len(data) / elapsed) / (1024 * 1024)
    print(f"  Received {len(received):,} bytes  {tick}")
    print(f"  {DIM}Throughput ≈ {mbps:.0f} MB/s  (wall time {elapsed:.3f}s){RESET}")


# ── Entry point ───────────────────────────────────────────────────────────────

def check_server():
    try:
        with socket.create_connection((HOST, PORT), timeout=2):
            pass
    except OSError:
        print(f"\n{RED}Server not reachable at {HOST}:{PORT}{RESET}")
        print("Start it first:")
        print("  ./build/bin/tcp_server        # local build")
        print("  docker compose up -d          # Docker")
        sys.exit(1)


if __name__ == "__main__":
    print(f"\n{BOLD}cpp-epoll-reactor-server demo{RESET}"
          f"  {DIM}→  {HOST}:{PORT}{RESET}")
    check_server()
    demo_echo()
    demo_concurrent()
    demo_large()
    print(f"\n{GREEN}{BOLD}All demos completed.{RESET}\n")
