#!/usr/bin/env python3
import argparse
import asyncio
import os
import tempfile
import time
from dataclasses import dataclass
from urllib.parse import urlparse
from typing import Optional, Tuple


class P2Quantile:
    def __init__(self, q: float):
        if not (0.0 < q < 1.0):
            raise ValueError("q must be in (0, 1)")
        self.q = q
        self._n = 0
        self._init = []
        self._qv = [0.0] * 5
        self._pos = [0] * 5
        self._npos = [0.0] * 5
        self._dn = [0.0] * 5

    def add(self, x: float) -> None:
        self._n += 1
        if self._n <= 5:
            self._init.append(x)
            if self._n == 5:
                self._init.sort()
                for i in range(5):
                    self._qv[i] = self._init[i]
                    self._pos[i] = i + 1
                self._npos[0] = 1.0
                self._npos[1] = 1.0 + 2.0 * self.q
                self._npos[2] = 1.0 + 4.0 * self.q
                self._npos[3] = 3.0 + 2.0 * self.q
                self._npos[4] = 5.0
                self._dn[0] = 0.0
                self._dn[1] = self.q / 2.0
                self._dn[2] = self.q
                self._dn[3] = (1.0 + self.q) / 2.0
                self._dn[4] = 1.0
            return

        k = 0
        if x < self._qv[0]:
            self._qv[0] = x
            k = 0
        elif x >= self._qv[4]:
            self._qv[4] = x
            k = 3
        else:
            for i in range(4):
                if self._qv[i] <= x < self._qv[i + 1]:
                    k = i
                    break

        for i in range(k + 1, 5):
            self._pos[i] += 1
        for i in range(5):
            self._npos[i] += self._dn[i]

        for i in (1, 2, 3):
            d = self._npos[i] - self._pos[i]
            if (d >= 1.0 and self._pos[i + 1] - self._pos[i] > 1) or (
                d <= -1.0 and self._pos[i - 1] - self._pos[i] < -1
            ):
                di = 1 if d > 0 else -1
                qp = self._parabolic(i, di)
                if self._qv[i - 1] < qp < self._qv[i + 1]:
                    self._qv[i] = qp
                else:
                    self._qv[i] = self._linear(i, di)
                self._pos[i] += di

    def _parabolic(self, i: int, d: int) -> float:
        n0, n1, n2 = self._pos[i - 1], self._pos[i], self._pos[i + 1]
        q0, q1, q2 = self._qv[i - 1], self._qv[i], self._qv[i + 1]
        return q1 + d / (n2 - n0) * (
            (n1 - n0 + d) * (q2 - q1) / (n2 - n1) + (n2 - n1 - d) * (q1 - q0) / (n1 - n0)
        )

    def _linear(self, i: int, d: int) -> float:
        return self._qv[i] + d * (self._qv[i + d] - self._qv[i]) / (self._pos[i + d] - self._pos[i])

    def result(self) -> float:
        if self._n == 0:
            return 0.0
        if self._n < 5:
            data = sorted(self._init)
            idx = (len(data) - 1) * self.q
            lo = int(idx)
            hi = min(lo + 1, len(data) - 1)
            w = idx - lo
            return data[lo] * (1.0 - w) + data[hi] * w
        return float(self._qv[2])


@dataclass
class Case:
    name: str
    method: str
    path: str
    download_to_disk: bool
    only_100_10: bool


class Stats:
    def __init__(self) -> None:
        self.total = 0
        self.success = 0
        self.lat_sum_ms = 0.0
        self.p95 = P2Quantile(0.95)
        self.p99 = P2Quantile(0.99)

    def record(self, ok: bool, latency_ms: float) -> None:
        self.total += 1
        if ok:
            self.success += 1
            self.lat_sum_ms += latency_ms
            self.p95.add(latency_ms)
            self.p99.add(latency_ms)


def _parse_base(base: str) -> Tuple[str, int]:
    u = urlparse(base if "://" in base else f"http://{base}")
    if u.scheme and u.scheme.lower() != "http":
        raise ValueError("仅支持HTTP协议（http://）")
    host = u.hostname or "127.0.0.1"
    port = u.port or 80
    return host, port


async def _http_request(
    host: str,
    port: int,
    method: str,
    path: str,
    *,
    timeout_s: float,
    download_dir: Optional[str],
) -> Tuple[bool, int]:
    reader = None
    writer = None
    tmp_path = None
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=timeout_s)
        req = (
            f"{method} {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Connection: close\r\n"
            "User-Agent: http-bench/1.0\r\n"
            "\r\n"
        )
        writer.write(req.encode("ascii", errors="strict"))
        await asyncio.wait_for(writer.drain(), timeout=timeout_s)

        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = await asyncio.wait_for(reader.read(4096), timeout=timeout_s)
            if not chunk:
                break
            buf += chunk

        if b"\r\n\r\n" in buf:
            head, rest = buf.split(b"\r\n\r\n", 1)
        else:
            head, rest = buf, b""

        first = head.split(b"\r\n", 1)[0].decode("iso-8859-1", errors="replace").strip()
        parts = first.split()
        status = 0
        if len(parts) >= 2 and parts[0].startswith("HTTP/"):
            try:
                status = int(parts[1])
            except ValueError:
                status = 0

        if download_dir is not None:
            fd, tmp_path = tempfile.mkstemp(prefix="bench_", suffix=".bin", dir=download_dir)
            with os.fdopen(fd, "wb") as f:
                if rest:
                    f.write(rest)
                while True:
                    chunk = await asyncio.wait_for(reader.read(65536), timeout=timeout_s)
                    if not chunk:
                        break
                    f.write(chunk)
        else:
            while True:
                chunk = await asyncio.wait_for(reader.read(65536), timeout=timeout_s)
                if not chunk:
                    break

        ok = 200 <= status < 300
        return ok, status
    except Exception:
        return False, 0
    finally:
        if writer is not None:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
        if tmp_path is not None:
            try:
                os.remove(tmp_path)
            except FileNotFoundError:
                pass
            except Exception:
                pass


async def _worker(
    stats: Stats,
    host: str,
    port: int,
    case: Case,
    end_t: float,
    *,
    timeout_s: float,
    download_dir: Optional[str],
) -> None:
    while True:
        now = time.monotonic()
        if now >= end_t:
            return
        t0 = now
        ok, _status = await _http_request(
            host,
            port,
            case.method,
            case.path,
            timeout_s=timeout_s,
            download_dir=download_dir if case.download_to_disk else None,
        )
        dt_ms = (time.monotonic() - t0) * 1000.0
        stats.record(ok, dt_ms)


async def run_one(
    host: str,
    port: int,
    case: Case,
    concurrency: int,
    duration_s: int,
    *,
    timeout_s: float,
) -> None:
    download_dir = None
    if case.download_to_disk:
        download_dir = tempfile.mkdtemp(prefix="http_bench_dl_")

    try:
        stats = Stats()
        t_start = time.monotonic()
        end_t = t_start + float(duration_s)
        tasks = [
            asyncio.create_task(
                _worker(stats, host, port, case, end_t, timeout_s=timeout_s, download_dir=download_dir)
            )
            for _ in range(concurrency)
        ]
        await asyncio.gather(*tasks)
        elapsed = float(duration_s)

        total = stats.total
        succ = stats.success
        fail = total - succ
        qps = total / elapsed if elapsed > 0 else 0.0
        succ_rate = (succ / total * 100.0) if total > 0 else 0.0
        p95 = stats.p95.result()
        p99 = stats.p99.result()

        print(f"\n=== {case.method} {case.path} | 并发={concurrency} | 持续={duration_s}s ===")
        print(f"总请求: {total}  成功: {succ}  失败: {fail}  成功率: {succ_rate:.2f}%")
        print(f"QPS: {qps:.2f}  P95: {p95:.2f}ms  P99: {p99:.2f}ms")
    finally:
        if download_dir is not None:
            try:
                for name in os.listdir(download_dir):
                    try:
                        os.remove(os.path.join(download_dir, name))
                    except Exception:
                        pass
                os.rmdir(download_dir)
            except Exception:
                pass


async def main_async(args: argparse.Namespace) -> None:
    host, port = _parse_base(args.base)
    print(f"目标: http://{host}:{port}")

    cases = [
        Case(name="root", method="GET", path="/", download_to_disk=False, only_100_10=False),
        Case(
            name="download_video",
            method="GET",
            path="/download/video/diangunfukua.mp4",
            download_to_disk=True,
            only_100_10=True,
        ),
        Case(
            name="download_image",
            method="GET",
            path="/download/images/hui.jpg",
            download_to_disk=True,
            only_100_10=True,
        ),
    ]

    conc_all = [100, 1000, 10000]
    dur_all = [10, 30, 60]

    for case in cases:
        if case.only_100_10:
            await run_one(host, port, case, 100, 10, timeout_s=args.timeout)
        else:
            for c in conc_all:
                for d in dur_all:
                    await run_one(host, port, case, c, d, timeout_s=args.timeout)


def main() -> None:
    parser = argparse.ArgumentParser(description="HTTP 压力测试 & QPS 测试（多路径矩阵）")
    parser.add_argument(
        "--base",
        default="http://127.0.0.1:8080",
        help="目标地址，如 http://127.0.0.1:8080",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=120.0,
        help="单次请求超时（秒）。下载接口建议较大",
    )
    args = parser.parse_args()
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
