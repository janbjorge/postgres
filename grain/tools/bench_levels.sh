#!/usr/bin/env bash
# The read path with minute, hour and day levels, simulated in plain SQL;
# there is no grain code.
#
# Question: with minute, hour and day levels and a 1-minute lag, how much
# faster than a plain GROUP BY are (1) hourly over 30 days, (2) daily over
# 30 days, (3) a total over a year? (1) tests the small tail, (2) and (3)
# test reading coarse levels. Every answer is checked against GROUP BY.
#
# Usage: bench_levels.sh OUTDIR SIZE...
#   Cluster: build-bench, port 54330; started and stopped if it isn't running.
#   SECS: seconds per timed variant (default 10).
#   REPEATS: run everything this many times, into OUTDIR/repK.
#   SQL: levels/*.sql.

source "$(dirname "$0")/lib.sh"
[ $# -ge 2 ] || usage
TOP=$1
shift
SQL=$TOOLS/levels

export O="timestamptz '2000-01-01 00:00:00+00'"           # bucket origin
export T_END="timestamptz '2026-10-19 12:30:17+00'"       # now
export W="timestamptz '2026-10-19 12:29:00+00'"           # watermark: date_bin(1 minute, T_END - lag 1 minute)
export START_HOUR="timestamptz '2026-09-19 12:00:00+00'"  # 30 days back, on the hour
export START_DAY="timestamptz '2026-09-19 00:00:00+00'"   # 30 days back, on the day
export START_YEAR="timestamptz '2025-10-19 00:00:00+00'"  # 365 days back, on the day

QUERIES="hourly30d daily30d total365d"
TUNED="-c work_mem=256MB -c max_parallel_workers_per_gather=8"

# The planner doesn't know the bucket lists are short and hash-joins whole
# level tables. A C reader looks rows up by key; make the SQL do the same.
GRAIN_PLAN="-c enable_hashjoin=off -c enable_mergejoin=off -c enable_seqscan=off"

# write_queries: OUT/q_N_QUERY_{b0,grain}.sql for each query.
write_queries() {
	local q
	for q in $QUERIES; do
		case $q in
		hourly30d) export START=$START_HOUR BUCKET="date_bin('1 hour', b, $O)" DAYS_OK=false ;;
		daily30d) export START=$START_DAY BUCKET="date_bin('1 day', b, $O)" DAYS_OK=true ;;
		total365d) export START=$START_YEAR BUCKET=$START_YEAR DAYS_OK=true ;;
		esac
		render "$SQL/b0_$q.sql" >"$OUT/q_${N}_${q}_b0.sql"
		render "$SQL/grain.sql" >"$OUT/q_${N}_${q}_grain.sql"
	done
}

# one_run SIZE...: everything, once, into OUT.
one_run() {
	local n q
	mkdir -p "$OUT"
	csv_header rows,query,variant,latency_ms,transactions,plan

	for n in "$@"; do
		export N=$n T=lv_$n
		echo "== $N rows"
		sql_file "$SQL/setup.sql"
		write_queries

		# Today, with the best index for each plan.
		for q in $QUERIES; do timed "$N,$q,b0_seq" "$OUT/q_${N}_${q}_b0.sql"; done
		for q in $QUERIES; do timed "$N,$q,b0_tuned" "$OUT/q_${N}_${q}_b0.sql" "$TUNED"; done
		sql -c "CREATE INDEX ${T}_cov ON $T (done_at) INCLUDE (bytes)" -c "VACUUM (ANALYZE) $T"
		for q in $QUERIES; do timed "$N,$q,b0_covering" "$OUT/q_${N}_${q}_b0.sql"; done
		sql -c "DROP INDEX ${T}_cov" -c "CREATE INDEX ${T}_brin ON $T USING brin (done_at)" -c "ANALYZE $T"
		for q in $QUERIES; do timed "$N,$q,b1_brin" "$OUT/q_${N}_${q}_b0.sql"; done

		# grain, all sealed buckets clean.
		sql -c "DROP INDEX ${T}_brin" -c "CREATE INDEX ${T}_bt ON $T (done_at)"
		sql_file "$SQL/seal.sql"
		for q in $QUERIES; do
			same "$N,$q,check_clean" "$OUT/q_${N}_${q}_b0.sql" "$OUT/q_${N}_${q}_grain.sql"
			timed "$N,$q,grain_clean" "$OUT/q_${N}_${q}_grain.sql" "$GRAIN_PLAN"
		done

		# grain, five sealed minutes marked.
		sql_file "$SQL/dirty5.sql"
		for q in $QUERIES; do
			same "$N,$q,check_dirty5" "$OUT/q_${N}_${q}_b0.sql" "$OUT/q_${N}_${q}_grain.sql"
			timed "$N,$q,grain_dirty5" "$OUT/q_${N}_${q}_grain.sql" "$GRAIN_PLAN"
		done

		echo "  sizes heap|levels: $(value "SELECT pg_size_pretty(pg_table_size('$T')) || '|' || pg_size_pretty(
			pg_table_size('${T}_min') + pg_table_size('${T}_hour') + pg_table_size('${T}_day'))")"
		sql -c "DROP TABLE $T, ${T}_min, ${T}_hour, ${T}_day, ${T}_dirty"
	done
}

use build-bench 54330
up $BENCH_SETTINGS

if [ -z "${REPEATS:-}" ]; then
	OUT=$TOP
	CSV=$OUT/results.csv
	one_run "$@"
else
	for k in $(seq 1 "$REPEATS"); do
		echo "### rep $k"
		OUT=$TOP/rep$k
		CSV=$OUT/results.csv
		one_run "$@"
	done
fi
