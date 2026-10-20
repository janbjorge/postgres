#!/usr/bin/env bash
# P1: the read ceiling (NOTES.md P1). Plain SQL, no grain code.
#
# Question: how much faster is a 30-day hourly count/sum/min/max when
# answered the way grain would (summary rows for sealed hours, a scan of
# the open hours and of the marked hours) than by a plain GROUP BY?
#
# Usage: bench_read.sh OUTDIR SIZE...
#   Cluster: build-bench, port 54330; started and stopped if it isn't running.
#   SECS: seconds per timed variant (default 10).
#   SQL: read/*.sql.

source "$(dirname "$0")/lib.sh"
[ $# -ge 2 ] || usage
OUT=$1
shift
CSV=$OUT/results.csv
SQL=$TOOLS/read

export O="timestamptz '2000-01-01 00:00:00+00'"      # bucket origin
export T_END="timestamptz '2026-10-19 12:30:00+00'"  # now
export START="timestamptz '2026-09-19 12:00:00+00'"  # 30 days back, on the hour
export W="timestamptz '2026-10-19 10:00:00+00'"      # watermark: date_bin(1 hour, T_END - 2 hours)
TUNED="-c work_mem=256MB -c max_parallel_workers_per_gather=8"

use build-bench 54330
up $BENCH_SETTINGS
mkdir -p "$OUT"
csv_header rows,variant,latency_ms,transactions,plan

for n in "$@"; do
	export N=$n T=downloads_$n
	b0=$OUT/q_${N}_b0.sql
	grain=$OUT/q_${N}_grain.sql
	echo "== $N rows"

	sql_file "$SQL/setup.sql"
	render "$SQL/b0.sql" >"$b0"
	render "$SQL/grain.sql" >"$grain"

	# Today, with the best index for each plan.
	timed "$N,b0_seq" "$b0"
	timed "$N,b0_tuned" "$b0" "$TUNED"
	sql -c "CREATE INDEX ${T}_bt ON $T (done_at)" -c "ANALYZE $T"
	timed "$N,b0_btree" "$b0"
	sql -c "DROP INDEX ${T}_bt" -c "CREATE INDEX ${T}_cov ON $T (done_at) INCLUDE (bytes)" -c "VACUUM (ANALYZE) $T"
	timed "$N,b0_covering" "$b0"
	sql -c "DROP INDEX ${T}_cov" -c "CREATE INDEX ${T}_brin ON $T USING brin (done_at)" -c "ANALYZE $T"
	timed "$N,b1_brin" "$b0"

	# grain, all sealed hours clean.
	sql -c "DROP INDEX ${T}_brin" -c "CREATE INDEX ${T}_bt ON $T (done_at)"
	sql_file "$SQL/seal.sql"
	same "$N,check_clean" "$b0" "$grain"
	timed "$N,grain_clean" "$grain"

	# grain, five sealed hours marked.
	sql_file "$SQL/dirty5.sql"
	same "$N,check_dirty5" "$b0" "$grain"
	timed "$N,grain_dirty5" "$grain"

	echo "  sizes heap|summary: $(value "SELECT pg_size_pretty(pg_table_size('$T')) || '|' || pg_size_pretty(pg_table_size('${T}_summary'))")"
	sql -c "DROP TABLE $T, ${T}_summary, ${T}_dirty"
done
