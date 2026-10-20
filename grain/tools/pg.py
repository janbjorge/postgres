"""Shared helpers for the grain benchmark scripts: one PostgreSQL build's
server, psql and pgbench, driven through subprocess. Standard library only.
"""

import csv
import os
import re
import subprocess
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
USER = "janbjorge"


def find(pattern: str, text: str) -> str:
    """The first group of pattern in text; an error if it isn't there."""
    m = re.search(pattern, text)
    if m is None:
        raise RuntimeError(f"no {pattern!r} in:\n{text}")
    return m.group(1)


@dataclass
class Server:
    build: str  # directory under ROOT, such as "build-bench"
    port: int
    data: str = "data"
    options: str = "-c client_min_messages=warning"

    @property
    def dir(self) -> Path:
        return ROOT / self.build

    def bin(self, name: str) -> str:
        return str(self.dir / "inst" / "bin" / name)

    def env(self, extra: str = "") -> dict:
        return {**os.environ, "PGHOST": str(self.dir), "PGPORT": str(self.port),
                "PGDATABASE": "postgres", "PGUSER": USER,
                "PGOPTIONS": f"{self.options} {extra}".strip()}

    def psql(self, sql: str, *args: str, extra: str = "") -> str:
        """Run SQL; stop on the first error; return stdout."""
        r = subprocess.run([self.bin("psql"), "-X", "-q", "-v", "ON_ERROR_STOP=1", *args],
                           input=sql, env=self.env(extra), capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"psql failed ({self.build}):\n{r.stderr}")
        return r.stdout

    def value(self, sql: str) -> str:
        return self.psql(sql, "-At").strip()

    def pgbench(self, script: Path, log: Path, *, secs: int | None = None,
                transactions: int | None = None, extra: str = "") -> tuple[str, str]:
        """Run one pgbench client; return (latency ms, transactions)."""
        limit = ["-T", str(secs)] if secs is not None else ["-t", str(transactions)]
        r = subprocess.run([self.bin("pgbench"), "-n", "-f", str(script), *limit],
                           env=self.env(extra), capture_output=True, text=True)
        log.write_text(r.stdout + r.stderr)
        if r.returncode != 0:
            raise RuntimeError(f"pgbench failed ({self.build}):\n{r.stderr}")
        return (find(r"latency average = ([0-9.]+) ms", r.stdout),
                find(r"number of transactions actually processed: ([0-9]+)", r.stdout))

    def start(self, settings: str = "") -> None:
        data = self.dir / self.data
        if not data.exists():
            subprocess.run([self.bin("initdb"), "-D", str(data), "-U", USER, "--no-sync"],
                           check=True, capture_output=True)
        opts = f"-p {self.port} -k {self.dir} -c listen_addresses='' {settings}"
        subprocess.run([self.bin("pg_ctl"), "-D", str(data), "-l", str(self.dir / f"server-{self.data}.log"),
                        "-w", "start", "-o", opts], check=True, capture_output=True)

    def stop(self) -> None:
        subprocess.run([self.bin("pg_ctl"), "-D", str(self.dir / self.data), "-w", "stop"],
                       check=True, capture_output=True)

    def running(self) -> bool:
        return subprocess.run([self.bin("pg_ctl"), "-D", str(self.dir / self.data), "status"],
                              capture_output=True).returncode == 0

    @contextmanager
    def up(self, settings: str = ""):
        """Start the cluster unless it runs; stop it after, if we started it."""
        started = not self.running()
        if started:
            self.start(settings)
        try:
            yield self
        finally:
            if started:
                self.stop()


# The settings every benchmark cluster runs with (NOTES.md P1).
BENCH_SETTINGS = "-c shared_buffers=2GB -c autovacuum=off -c timezone=UTC"

SCAN = re.compile(r"(Seq Scan|Parallel Seq Scan|Index Only Scan|Index Scan|Bitmap Heap Scan|Bitmap Index Scan)"
                  r" (using \S+ )?on \S+")


def plan_scans(server: Server, query: str, out: Path, extra: str = "") -> str:
    """EXPLAIN ANALYZE the query into out; return its scans, sorted, ';'-joined."""
    text = server.psql(f"EXPLAIN (ANALYZE, BUFFERS, COSTS OFF) {query}", extra=extra)
    out.write_text(text)
    return ";".join(sorted({m.group(0) for m in SCAN.finditer(text)}))


class Csv:
    """Append rows to results.csv, writing the header if the file is new."""

    def __init__(self, path: Path, header: list[str]):
        self.path = path
        if not path.exists():
            with path.open("w", newline="") as f:
                csv.writer(f).writerow(header)

    def row(self, *values) -> None:
        with self.path.open("a", newline="") as f:
            csv.writer(f).writerow(values)


def check_equal(server: Server, a: str, b: str) -> tuple[int, int]:
    """Rows of a, and rows that differ between a and b as multisets."""
    differ = int(server.value(f"SELECT count(*) FROM ((({a}) EXCEPT ALL ({b})) UNION ALL (({b}) EXCEPT ALL ({a}))) x"))
    rows = int(server.value(f"SELECT count(*) FROM ({a}) x"))
    return rows, differ
