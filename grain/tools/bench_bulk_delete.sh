#!/usr/bin/env bash
# One DELETE of 1M rows, on a freshly built table every time.
#
# Question: what does the delete callback cost per row, with and without an
# index that asks for it? bench_delete.sh's bulk_delete rolls back and
# repeats on the same table, so later runs pay for checking the aborted
# deletes and hide the callback.
#
# Usage: bench_bulk_delete.sh OUTDIR ROUNDS (servers as in bench_delete.sh)
set -euo pipefail

OUT=$1; ROUNDS=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT"
CSV=$OUT/results.csv
echo "round,server,workload,latency_ms,transactions" > "$CSV"
export PGDATABASE=postgres PGUSER=${PGUSER:-$USER} PGOPTIONS="-c client_min_messages=warning"

dir() { case $1 in master) echo "$ROOT/build-bench" ;; patch) echo "$ROOT/build-bench-patch" ;; esac; }
port() { case $1 in master) echo 54330 ;; patch) echo 54331 ;; esac; }
psql_() { local s=$1; shift; "$(dir "$s")/inst/bin/psql" -X -q -v ON_ERROR_STOP=1 -h "$(dir "$s")" -p "$(port "$s")" "$@"; }

for s in master patch; do
	"$(dir "$s")/inst/bin/pg_ctl" -D "$(dir "$s")/data-f" -l "$(dir "$s")/server-f.log" -w start -o \
		"-p $(port "$s") -k $(dir "$s") -c listen_addresses='' -c shared_buffers=2GB -c autovacuum=off -c timezone=UTC -c max_wal_size=8GB" >/dev/null
done

run() {	# run ROUND SERVER NOTIFY
	local r=$1 s=$2 notify=$3 ms idx=""
	[ "$notify" = yes ] && idx="CREATE INDEX ON fd_bulk USING dummy_index_am (v);"
	psql_ "$s" <<-SQL
	DROP TABLE IF EXISTS fd_bulk;
	CREATE TABLE fd_bulk (id int4 PRIMARY KEY, k int4 NOT NULL, v int4 NOT NULL);
	INSERT INTO fd_bulk SELECT i, i, i FROM generate_series(1, 1000000) i;
	CREATE INDEX ON fd_bulk (k);
	$idx
	VACUUM (ANALYZE) fd_bulk;
	CHECKPOINT;
	SQL
	ms=$(psql_ "$s" -c '\timing on' -c 'DELETE FROM fd_bulk' | rg -o 'Time: ([0-9.]+) ms' -r '$1')
	local w=bulk_delete_fresh_fd_plain
	[ "$notify" = yes ] && w=bulk_delete_fresh_fd_notify
	echo "$r,$s,$w,$ms,1" >> "$CSV"
	echo "  round $r $s $w: $ms ms"
}

for r in $(seq 1 "$ROUNDS"); do
	run "$r" master no
	run "$r" patch no
	run "$r" patch yes
done

for s in master patch; do "$(dir "$s")/inst/bin/pg_ctl" -D "$(dir "$s")/data-f" -w stop >/dev/null; done
echo done
