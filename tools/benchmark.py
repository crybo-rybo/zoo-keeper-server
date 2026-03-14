#!/usr/bin/env python3
"""
Benchmark harness for zoo-keeper-server.

Fires N concurrent stateless POST /v1/chat/completions requests and reports
throughput and latency percentiles.

Usage:
    python tools/benchmark.py --url http://127.0.0.1:8080 --model local-model \\
        --requests 100 --concurrency 4

Requires Python 3.9+ and only stdlib modules.
"""

from __future__ import annotations

import argparse
import http.client
import json
import statistics
import threading
import time
import urllib.parse
from dataclasses import dataclass, field


@dataclass
class RequestResult:
    latency_ms: float
    status: int
    error: str | None = None


@dataclass
class BenchmarkResults:
    total_requests: int = 0
    errors: int = 0
    total_seconds: float = 0.0
    latencies_ms: list[float] = field(default_factory=list)
    first_error: str | None = None


def send_request(
    host: str,
    port: int,
    path: str,
    body: bytes,
    headers: dict[str, str],
    use_tls: bool,
) -> RequestResult:
    start = time.monotonic()
    try:
        if use_tls:
            conn = http.client.HTTPSConnection(host, port, timeout=30)
        else:
            conn = http.client.HTTPConnection(host, port, timeout=30)
        conn.request("POST", path, body=body, headers=headers)
        resp = conn.getresponse()
        resp_body = resp.read().decode("utf-8", errors="replace")
        elapsed_ms = (time.monotonic() - start) * 1000
        error = resp_body if resp.status >= 400 else None
        return RequestResult(latency_ms=elapsed_ms, status=resp.status, error=error)
    except Exception as e:
        elapsed_ms = (time.monotonic() - start) * 1000
        return RequestResult(latency_ms=elapsed_ms, status=0, error=str(e))
    finally:
        try:
            conn.close()
        except Exception:
            pass


def worker(
    work_queue: list[int],
    lock: threading.Lock,
    results: list[RequestResult],
    host: str,
    port: int,
    path: str,
    body: bytes,
    headers: dict[str, str],
    use_tls: bool,
) -> None:
    while True:
        with lock:
            if not work_queue:
                return
            work_queue.pop()
        result = send_request(host, port, path, body, headers, use_tls)
        with lock:
            results.append(result)


def percentile(sorted_data: list[float], p: float) -> float:
    if not sorted_data:
        return 0.0
    k = (len(sorted_data) - 1) * (p / 100.0)
    f = int(k)
    c = f + 1
    if c >= len(sorted_data):
        return sorted_data[f]
    return sorted_data[f] + (k - f) * (sorted_data[c] - sorted_data[f])


def run_benchmark(
    url: str,
    model: str,
    total_requests: int,
    concurrency: int,
    api_key: str | None,
    prompt: str,
) -> BenchmarkResults:
    parsed = urllib.parse.urlparse(url)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    path = "/v1/chat/completions"
    use_tls = parsed.scheme == "https"

    request_body = json.dumps(
        {
            "model": model,
            "messages": [{"role": "user", "content": prompt}],
            "stream": False,
        }
    ).encode("utf-8")

    headers: dict[str, str] = {"Content-Type": "application/json"}
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    work_queue = list(range(total_requests))
    lock = threading.Lock()
    results: list[RequestResult] = []

    threads = []
    start_time = time.monotonic()

    for _ in range(min(concurrency, total_requests)):
        t = threading.Thread(
            target=worker,
            args=(work_queue, lock, results, host, port, path, request_body, headers, use_tls),
        )
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    total_seconds = time.monotonic() - start_time

    bench = BenchmarkResults(
        total_requests=len(results),
        total_seconds=total_seconds,
    )

    for r in results:
        bench.latencies_ms.append(r.latency_ms)
        if r.status == 0 or r.status >= 400:
            bench.errors += 1
            if bench.first_error is None and r.error:
                bench.first_error = r.error[:200]

    bench.latencies_ms.sort()
    return bench


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Benchmark harness for zoo-keeper-server"
    )
    parser.add_argument(
        "--url",
        required=True,
        help="Server base URL (e.g., http://127.0.0.1:8080)",
    )
    parser.add_argument("--model", required=True, help="Model ID to use in requests")
    parser.add_argument(
        "--requests", type=int, default=100, help="Total requests to send (default: 100)"
    )
    parser.add_argument(
        "--concurrency", type=int, default=4, help="Number of concurrent workers (default: 4)"
    )
    parser.add_argument("--api-key", default=None, help="API key for Authorization header")
    parser.add_argument(
        "--prompt",
        default="Reply with the single word: zoo.",
        help='Prompt to send (default: "Reply with the single word: zoo.")',
    )
    args = parser.parse_args()

    if args.requests < 1:
        parser.error("--requests must be >= 1")
    if args.concurrency < 1:
        parser.error("--concurrency must be >= 1")

    print(
        f"Benchmarking {args.url} with {args.requests} requests, "
        f"{args.concurrency} workers...\n"
    )

    bench = run_benchmark(
        url=args.url,
        model=args.model,
        total_requests=args.requests,
        concurrency=args.concurrency,
        api_key=args.api_key,
        prompt=args.prompt,
    )

    throughput = bench.total_requests / bench.total_seconds if bench.total_seconds > 0 else 0
    p50 = percentile(bench.latencies_ms, 50)
    p95 = percentile(bench.latencies_ms, 95)
    p99 = percentile(bench.latencies_ms, 99)

    print(f"Requests:      {bench.total_requests} total, {bench.errors} errors")
    print(f"Concurrency:   {args.concurrency} workers")
    print(f"Total time:    {bench.total_seconds:.2f}s")
    print(f"Throughput:    {throughput:.2f} req/s")
    print(f"Latency p50:   {p50:.0f}ms")
    print(f"Latency p95:   {p95:.0f}ms")
    print(f"Latency p99:   {p99:.0f}ms")

    if bench.first_error:
        print(f"\nFirst error:   {bench.first_error}")


if __name__ == "__main__":
    main()
