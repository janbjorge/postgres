#!/usr/bin/env bash
# What the delete callback costs DELETE and UPDATE.
#
# Question: (1) on a table with no index that asks for the callback, is the
# patched server as fast as master? (2) With one index that asks for it
# (dummy_index_am), what does fetching the old row and calling the index
# cost per row?
#
# master is build-bench/ (port 54330); the patch is build-bench-patch/
# (port 54331); both -O2 without asserts, same settings. Rounds alternate
# between the two so drift hits both.
#
# Usage: bench_delete.sh OUTDIR ROUNDS
set -euo pipefail

OUT=$1; ROUNDS=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SECS=${SECS:-10}
ROWS=1000000
mkdir -p "$OUT"
CSV=$OUT/results.csv
echo "round,server,workload,latency_ms,transactions" > "$CSV"
export PGDATABASE=postgres PGUSER=${PGUSER:-$USER} PGOPTIONS="-c client_min_messages=warning"

dir() { case $1 in master) echo "$ROOT/build-bench" ;; patch) echo "$ROOT/build-bench-patch" ;; esac; }
port() { case $1 in master) echo 54330 ;; patch) echo 54331 ;; esac; }
psql_() { local s=$1; shift; "$(dir "$s")/inst/bin/psql" -X -q -v ON_ERROR_STOP=1 -h "$(dir "$s")" -p "$(port "$s")" "$@"; }

for s in master patch; do
	d=$(dir "$s")
	[ -d "$d/data-f" ] || "$d/inst/bin/initdb" -D "$d/data-f" -U "$PGUSER" --no-sync >/dev/null
	"$d/inst/bin/pg_ctl" -D "$d/data-f" -l "$d/server-f.log" -w start -o \
		"-p $(port "$s") -k $d -c listen_addresses='' -c shared_buffers=2GB -c autovacuum=off -c timezone=UTC -c max_wal_size=8GB" >/dev/null
	psql_ "$s" <<-SQL
	DROP TABLE IF EXISTS fd_plain, fd_notify;
	CREATE TABLE fd_plain (id int4 PRIMARY KEY, k int4 NOT NULL, v int4 NOT NULL);
	INSERT INTO fd_plain SELECT i, i, i FROM generate_series(1, $ROWS) i;
	CREATE INDEX ON fd_plain (k);
	VACUUM (ANALYZE) fd_plain;
	SQL
	if [ "$s" = patch ]; then
		psql_ "$s" <<-SQL
		CREATE EXTENSION IF NOT EXISTS dummy_index_am;
		CREATE TABLE fd_notify (LIKE fd_plain INCLUDING ALL);
		INSERT INTO fd_notify SELECT * FROM fd_plain;
		CREATE INDEX ON fd_notify USING dummy_index_am (v);
		VACUUM (ANALYZE) fd_notify;
		SQL
	fi
done

# Workloads. bulk_*: one statement over every row, rolled back, so the
# table stays the same. row_*: one row per transaction, kept the same size.
cat > "$OUT/bulk_delete.sql" <<-'SQL'
BEGIN;
DELETE FROM TBL;
ROLLBACK;
SQL
cat > "$OUT/bulk_update.sql" <<-'SQL'
BEGIN;
UPDATE TBL SET k = k + 1;
ROLLBACK;
SQL
cat > "$OUT/row_delete_insert.sql" <<-'SQL'
\set id random(1, 1000000)
BEGIN;
DELETE FROM TBL WHERE id = :id;
INSERT INTO TBL VALUES (:id, :id, :id);
COMMIT;
SQL
cat > "$OUT/row_update.sql" <<-'SQL'
\set id random(1, 1000000)
UPDATE TBL SET k = k + 1 WHERE id = :id;
SQL

run() {	# run ROUND SERVER TABLE WORKLOAD
	local r=$1 s=$2 t=$3 w=$4 f log lat tx args
	f=$OUT/${w}_${t}.sql
	sed "s/TBL/$t/" "$OUT/$w.sql" > "$f"
	case $w in bulk_*) args="-t 3" ;; *) args="-T $SECS" ;; esac
	psql_ "$s" -c "VACUUM $t" -c CHECKPOINT
	log=$OUT/pgbench_${r}_${s}_${t}_${w}.log
	"$(dir "$s")/inst/bin/pgbench" -h "$(dir "$s")" -p "$(port "$s")" -n -f "$f" $args > "$log" 2>&1
	lat=$(rg -o 'latency average = ([0-9.]+) ms' -r '$1' "$log")
	tx=$(rg -o 'number of transactions actually processed: ([0-9]+)' -r '$1' "$log")
	echo "$r,$s,${w}_${t},$lat,$tx" >> "$CSV"
	echo "  round $r $s ${w}_$t: $lat ms ($tx)"
}

WORKLOADS="bulk_delete bulk_update row_delete_insert row_update"
for r in $(seq 1 "$ROUNDS"); do
	for w in $WORKLOADS; do
		for s in master patch; do run "$r" "$s" fd_plain "$w"; done
		run "$r" patch fd_notify "$w"
	done
done

for s in master patch; do "$(dir "$s")/inst/bin/pg_ctl" -D "$(dir "$s")/data-f" -w stop >/dev/null; done
echo done
