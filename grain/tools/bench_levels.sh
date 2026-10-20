#!/usr/bin/env bash
# The read path, simulated in plain SQL: no grain code.
#
# Question: with minute, hour and day levels and a 1-minute lag, how much
# faster than a plain GROUP BY are (1) hourly over 30 days, (2) daily over
# 30 days, (3) a total over a year? (1) tests the small tail, (2) and (3)
# test reading coarse levels. Every answer is checked against GROUP BY.
#
# Usage: bench_levels.sh OUTDIR SIZE...
# Needs the build-bench cluster running on port 54330.
set -euo pipefail

OUT=$1; shift
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=$ROOT/build-bench/inst/bin
export PGHOST=$ROOT/build-bench PGPORT=54330 PGDATABASE=postgres PGUSER=${PGUSER:-$USER}
export PGOPTIONS="-c client_min_messages=warning"
PSQL="$BIN/psql -X -q -v ON_ERROR_STOP=1"
SECS=${SECS:-10}
mkdir -p "$OUT"
CSV=$OUT/results.csv
[ -f "$CSV" ] || echo "rows,query,variant,latency_ms,transactions,plan" > "$CSV"

O="timestamptz '2000-01-01 00:00:00+00'"
T_END="timestamptz '2026-10-19 12:30:17+00'"
W="timestamptz '2026-10-19 12:29:00+00'"        # date_bin(1 minute, T_END - lag 1 minute)
F_HOUR="timestamptz '2026-09-19 12:00:00+00'"   # 30 days, on the hour
F_DAY="timestamptz '2026-09-19 00:00:00+00'"    # 30 days, on the day
F_YEAR="timestamptz '2025-10-19 00:00:00+00'"   # 365 days, on the day

QUERIES="hourly30d daily30d total365d"

# The planner doesn't know the bucket lists are short and hash-joins whole
# level tables. A C reader looks rows up by key; make the SQL do the same.
GRAIN_PLAN="-c enable_hashjoin=off -c enable_mergejoin=off -c enable_seqscan=off"

run() {	# run SIZE QUERY VARIANT FILE
	local n=$1 q=$2 v=$3 f=$4 lat tx plan
	$BIN/pgbench -n -f "$f" -T 2 >/dev/null 2>&1	# warm up
	$BIN/pgbench -n -f "$f" -T "$SECS" > "$OUT/pgbench_${n}_${q}_${v}.log" 2>&1
	lat=$(rg -o 'latency average = ([0-9.]+) ms' -r '$1' "$OUT/pgbench_${n}_${q}_${v}.log")
	tx=$(rg -o 'number of transactions actually processed: ([0-9]+)' -r '$1' "$OUT/pgbench_${n}_${q}_${v}.log")
	$PSQL -c "EXPLAIN (ANALYZE, BUFFERS, COSTS OFF) $(cat "$f")" > "$OUT/plan_${n}_${q}_${v}.txt"
	plan=$(rg -o '(Seq Scan|Parallel Seq Scan|Index Only Scan|Index Scan|Bitmap Heap Scan) (using \S+ )?on \S+' "$OUT/plan_${n}_${q}_${v}.txt" | sort -u | paste -sd';' -)
	echo "$n,$q,$v,$lat,$tx,\"$plan\"" >> "$CSV"
	echo "  $q $v: $lat ms ($tx runs)"
}

check() {	# check SIZE QUERY LABEL
	local n=$1 q=$2 b g diff rows
	b=$(cat "$OUT/q_${n}_${q}_b0.sql"); g=$(cat "$OUT/q_${n}_${q}_grain.sql")
	diff=$($PSQL -At -c "SELECT count(*) FROM ((($b) EXCEPT ALL ($g)) UNION ALL (($g) EXCEPT ALL ($b))) x")
	rows=$($PSQL -At -c "SELECT count(*) FROM ($b) x")
	echo "$n,$q,check_$3,,,\"$rows buckets; $diff differ\"" >> "$CSV"
	echo "  check $q $3: $rows buckets, $diff differ"
	[ "$diff" = 0 ] || { echo "MISMATCH at $n $q $3" >&2; exit 1; }
}

b0_query() {	# b0_query TABLE QUERY -> plain GROUP BY
	local t=$1
	case $2 in
	hourly30d) echo "SELECT date_bin('1 hour', done_at, $O) AS bucket, count(*) AS n, sum(bytes) AS s, min(bytes) AS mn, max(bytes) AS mx FROM $t WHERE done_at >= $F_HOUR AND done_at < $T_END GROUP BY 1 ORDER BY 1" ;;
	daily30d) echo "SELECT date_bin('1 day', done_at, $O) AS bucket, count(*) AS n, sum(bytes) AS s, min(bytes) AS mn, max(bytes) AS mx FROM $t WHERE done_at >= $F_DAY AND done_at < $T_END GROUP BY 1 ORDER BY 1" ;;
	total365d) echo "SELECT $F_YEAR AS bucket, count(*) AS n, sum(bytes) AS s, min(bytes) AS mn, max(bytes) AS mx FROM $t WHERE done_at >= $F_YEAR AND done_at < $T_END" ;;
	esac
}

grain_query() {	# grain_query TABLE QUERY -> grain's read path over three levels
	local t=$1 f outb daysok
	case $2 in
	hourly30d) f=$F_HOUR; outb="date_bin('1 hour', b, $O)"; daysok=false ;;
	daily30d) f=$F_DAY; outb="date_bin('1 day', b, $O)"; daysok=true ;;
	total365d) f=$F_YEAR; outb="$F_YEAR"; daysok=true ;;
	esac
	# Top down: a day row where the output is daily or coarser, the day is
	# sealed and has no marker; else its hour rows where sealed and clean;
	# else minute rows where sealed and clean; else the rows themselves (the
	# tail after W, and every dirty minute). One statement, one snapshot.
	echo "SELECT $outb AS bucket, sum(n)::int8 AS n, sum(s)::int8 AS s, min(mn) AS mn, max(mx) AS mx FROM (WITH mk AS (SELECT bucket FROM ${t}_dirty WHERE bucket >= $f AND bucket < $W), dd AS (SELECT d, ($daysok AND d + interval '1 day' <= $W AND NOT EXISTS (SELECT 1 FROM mk WHERE mk.bucket >= d AND mk.bucket < d + interval '1 day')) AS ok FROM generate_series($f, $T_END - interval '1 microsecond', interval '1 day') d), hh AS (SELECT h, (h + interval '1 hour' <= $W AND NOT EXISTS (SELECT 1 FROM mk WHERE mk.bucket >= h AND mk.bucket < h + interval '1 hour')) AS ok FROM dd, generate_series(dd.d, dd.d + interval '23 hours', interval '1 hour') h WHERE NOT dd.ok AND h < $T_END), mm AS (SELECT m FROM hh, generate_series(hh.h, hh.h + interval '59 minutes', interval '1 minute') m WHERE NOT hh.ok AND m + interval '1 minute' <= $W AND NOT EXISTS (SELECT 1 FROM mk WHERE mk.bucket = m)) SELECT g.bucket AS b, g.n, g.s, g.mn, g.mx FROM ${t}_day g JOIN dd ON g.bucket = dd.d WHERE dd.ok UNION ALL SELECT g.bucket, g.n, g.s, g.mn, g.mx FROM ${t}_hour g JOIN hh ON g.bucket = hh.h WHERE hh.ok UNION ALL SELECT g.bucket, g.n, g.s, g.mn, g.mx FROM ${t}_min g JOIN mm ON g.bucket = mm.m UNION ALL SELECT date_bin('1 minute', done_at, $O), count(*), sum(bytes), min(bytes), max(bytes) FROM $t WHERE done_at >= $W AND done_at < $T_END GROUP BY 1 UNION ALL SELECT k.bucket, count(*), sum(r.bytes), min(r.bytes), max(r.bytes) FROM (SELECT DISTINCT bucket FROM mk) k JOIN $t r ON r.done_at >= k.bucket AND r.done_at < k.bucket + interval '1 minute' GROUP BY 1) x GROUP BY 1 HAVING sum(n) > 0 ORDER BY 1"
}

for N in "$@"; do
	T=lv_$N
	echo "== $N rows"
	$PSQL -o /dev/null <<-SQL
	DROP TABLE IF EXISTS $T, ${T}_min, ${T}_hour, ${T}_day, ${T}_dirty;
	CREATE TABLE $T (done_at timestamptz NOT NULL, bytes int4 NOT NULL, status int2 NOT NULL);
	SELECT setseed(0.42);
	INSERT INTO $T
	  SELECT $T_END - interval '400 days' + (i - 1) * (interval '400 days' / $N),
	         (random() * 1000000)::int4, (i % 5)::int2
	  FROM generate_series(1, $N) i;
	VACUUM (ANALYZE) $T;
	SQL
	for q in $QUERIES; do
		b0_query "$T" "$q" > "$OUT/q_${N}_${q}_b0.sql"
		grain_query "$T" "$q" > "$OUT/q_${N}_${q}_grain.sql"
	done

	for q in $QUERIES; do run "$N" "$q" b0_seq "$OUT/q_${N}_${q}_b0.sql"; done
	for q in $QUERIES; do
		PGOPTIONS="$PGOPTIONS -c work_mem=256MB -c max_parallel_workers_per_gather=8" run "$N" "$q" b0_tuned "$OUT/q_${N}_${q}_b0.sql"
	done
	$PSQL -c "CREATE INDEX ${T}_cov ON $T (done_at) INCLUDE (bytes)" -c "VACUUM (ANALYZE) $T"
	for q in $QUERIES; do run "$N" "$q" b0_covering "$OUT/q_${N}_${q}_b0.sql"; done
	$PSQL -c "DROP INDEX ${T}_cov" -c "CREATE INDEX ${T}_brin ON $T USING brin (done_at)" -c "ANALYZE $T"
	for q in $QUERIES; do run "$N" "$q" b1_brin "$OUT/q_${N}_${q}_b0.sql"; done

	# grain's state, as the sealer would leave it: a row for every sealed
	# minute (empty ones with n = 0), hours folded from minutes, days from
	# hours; no markers.
	$PSQL -o /dev/null <<-SQL
	DROP INDEX ${T}_brin;
	CREATE INDEX ${T}_bt ON $T (done_at);
	CREATE TABLE ${T}_min (bucket timestamptz PRIMARY KEY, n int8, s int8, mn int4, mx int4);
	CREATE TABLE ${T}_hour (LIKE ${T}_min INCLUDING ALL);
	CREATE TABLE ${T}_day (LIKE ${T}_min INCLUDING ALL);
	CREATE TABLE ${T}_dirty (bucket timestamptz NOT NULL);
	CREATE INDEX ON ${T}_dirty (bucket);
	INSERT INTO ${T}_min
	  SELECT m, coalesce(a.n, 0), coalesce(a.s, 0), a.mn, a.mx
	  FROM generate_series(date_bin('1 minute', $T_END - interval '400 days', $O),
	                       $W - interval '1 minute', interval '1 minute') m
	  LEFT JOIN (SELECT date_bin('1 minute', done_at, $O) AS b, count(*) AS n, sum(bytes) AS s,
	                    min(bytes) AS mn, max(bytes) AS mx
	             FROM $T WHERE done_at < $W GROUP BY 1) a ON a.b = m;
	INSERT INTO ${T}_hour
	  SELECT date_bin('1 hour', bucket, $O), sum(n), sum(s), min(mn), max(mx) FROM ${T}_min
	  GROUP BY 1 HAVING date_bin('1 hour', bucket, $O) + interval '1 hour' <= $W;
	INSERT INTO ${T}_day
	  SELECT date_bin('1 day', bucket, $O), sum(n), sum(s), min(mn), max(mx) FROM ${T}_hour
	  GROUP BY 1 HAVING date_bin('1 day', bucket, $O) + interval '1 day' <= $W;
	VACUUM (ANALYZE) $T, ${T}_min, ${T}_hour, ${T}_day, ${T}_dirty;
	SQL

	for q in $QUERIES; do check "$N" "$q" clean; PGOPTIONS="$PGOPTIONS $GRAIN_PLAN" run "$N" "$q" grain_clean "$OUT/q_${N}_${q}_grain.sql"; done

	# Five late rows into five sealed minutes, each with its marker, as
	# aminsert would write; the hour and day rolled up above are now stale.
	# One carries bytes above every other row, so a missed marker shows in max.
	$PSQL -o /dev/null <<-SQL
	INSERT INTO $T
	  SELECT $F_HOUR + k * interval '100 hours' + interval '30 minutes 17 seconds', 7777777, 0
	  FROM generate_series(1, 5) k;
	INSERT INTO ${T}_dirty
	  SELECT date_bin('1 minute', $F_HOUR + k * interval '100 hours' + interval '30 minutes 17 seconds', $O)
	  FROM generate_series(1, 5) k;
	ANALYZE $T, ${T}_dirty;
	SQL
	for q in $QUERIES; do check "$N" "$q" dirty5; PGOPTIONS="$PGOPTIONS $GRAIN_PLAN" run "$N" "$q" grain_dirty5 "$OUT/q_${N}_${q}_grain.sql"; done

	$PSQL -At -c "SELECT pg_size_pretty(pg_table_size('$T')), pg_size_pretty(pg_table_size('${T}_min') + pg_table_size('${T}_hour') + pg_table_size('${T}_day'))" | sed "s/^/  sizes heap|levels: /"
	$PSQL -c "DROP TABLE $T, ${T}_min, ${T}_hour, ${T}_day, ${T}_dirty"
done
