# Shared helpers for the grain benchmark scripts. Source it; don't run it.
#
# Each script sets OUT (its output directory) and CSV (OUT/results.csv),
# picks a cluster with `use`, and starts it with `up`. SQL lives in .sql
# files; {{NAME}} in them is filled from the exported shell variable NAME
# by `render`. Works with macOS's bash 3.2.

set -euo pipefail

TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$TOOLS/../.." && pwd)
SECS=${SECS:-10}
DBUSER=$(whoami)

# The settings every benchmark cluster runs with.
BENCH_SETTINGS="-c shared_buffers=2GB -c autovacuum=off -c timezone=UTC"

die() { echo "$*" >&2; exit 1; }

# usage: print the script's header comment, and exit.
usage() { sed -n '2,/^[^#]/s/^# \{0,1\}//p' "$0" >&2; exit 2; }

# use BUILD PORT [DATA]: point psql, pgbench and pg_ctl at one cluster.
use() {
	BUILD=$ROOT/$1
	PORT=$2
	DATA=$BUILD/${3:-data}
	BIN=$BUILD/inst/bin
	export PGHOST=$BUILD PGPORT=$PORT PGDATABASE=postgres PGUSER=$DBUSER
	export PGOPTIONS="-c client_min_messages=warning"
}

# up [SETTINGS...]: start the current cluster unless it is running. A
# cluster started here is stopped when the script exits; one you started
# yourself is left running.
STARTED=""
up() {
	"$BIN/pg_ctl" -D "$DATA" status >/dev/null 2>&1 && return
	[ -d "$DATA" ] || "$BIN/initdb" -D "$DATA" -U "$DBUSER" --no-sync >/dev/null
	"$BIN/pg_ctl" -D "$DATA" -l "$BUILD/server-$(basename "$DATA").log" -w start \
		-o "-p $PORT -k $BUILD -c listen_addresses='' $*" >/dev/null
	STARTED="$STARTED $DATA"
}
stop_started() {
	local d
	for d in $STARTED; do
		"$(dirname "$d")/inst/bin/pg_ctl" -D "$d" -w stop >/dev/null
	done
}
trap stop_started EXIT

# sql [PSQL ARGS...]: psql on the current cluster; stops at the first error.
sql() { "$BIN/psql" -X -q -v ON_ERROR_STOP=1 "$@"; }

# value QUERY: one value, unaligned.
value() { sql -At -c "$1"; }

# render FILE: FILE with each {{NAME}} replaced by $NAME; NAME must be exported.
render() {
	perl -pe 's/\{\{(\w+)\}\}/$ENV{$1} \/\/ die "render: \$$1 is not set, in $ARGV\n"/ge' "$1"
}

# sql_file FILE: render FILE and run it, discarding its output.
sql_file() { render "$1" | sql -o /dev/null; }

# pick REGEX FILE: the first group of REGEX's first match in FILE.
pick() { rg -o -m1 -r '$1' "$1" "$2" || die "no /$1/ in $2"; }

# csv_header COLUMNS: start CSV with COLUMNS, unless it exists already.
csv_header() { [ -f "$CSV" ] || echo "$1" >"$CSV"; }

# pgb LOG PGBENCH_ARGS...: run pgbench into LOG; sets LAT (ms) and TX (runs).
pgb() {
	local log=$1
	shift
	"$BIN/pgbench" -n "$@" >"$log" 2>&1 || die "pgbench failed, see $log"
	LAT=$(pick 'latency average = ([0-9.]+) ms' "$log")
	TX=$(pick 'number of transactions actually processed: ([0-9]+)' "$log")
}

# timed KEY QUERY_FILE [PGOPTIONS]: warm up for 2 s, time for SECS s, and
# EXPLAIN ANALYZE the query once. KEY is the leading CSV fields, such as
# "1000,hourly30d,b0_seq"; the row gets latency, runs and the plan's scans.
timed() {
	local key=$1 query=$2 name=${1//,/_} plan scans
	local -x PGOPTIONS="$PGOPTIONS ${3:-}"
	pgb "$OUT/warmup.log" -f "$query" -T 2
	pgb "$OUT/pgbench_$name.log" -f "$query" -T "$SECS"
	plan=$OUT/plan_$name.txt
	{ echo "EXPLAIN (ANALYZE, BUFFERS, COSTS OFF)"; cat "$query"; } | sql >"$plan"
	scans=$(rg -o '(Seq Scan|Parallel Seq Scan|Index Only Scan|Index Scan|Bitmap Heap Scan|Bitmap Index Scan) (using \S+ )?on \S+' "$plan" |
		sort -u | paste -sd';' -)
	echo "$key,$LAT,$TX,\"$scans\"" >>"$CSV"
	echo "  $key: $LAT ms ($TX runs)"
}

# same KEY QUERY_A QUERY_B: do the two queries return the same rows (as
# multisets)? Adds a CSV row; exits on any difference.
same() {
	local key=$1 a b rows differ
	a=$(sed '$ s/;$//' "$2")
	b=$(sed '$ s/;$//' "$3")
	rows=$(value "SELECT count(*) FROM ($a) x")
	differ=$(value "SELECT count(*) FROM ((($a) EXCEPT ALL ($b)) UNION ALL (($b) EXCEPT ALL ($a))) x")
	echo "$key,,,\"$rows buckets; $differ differ\"" >>"$CSV"
	echo "  $key: $rows buckets, $differ differ"
	[ "$differ" = 0 ] || die "MISMATCH at $key"
}
