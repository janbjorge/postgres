#!/usr/bin/env bash
# What the delete callback (amnotifydelete) costs DELETE and UPDATE.
#
# Question: (1) on a table with no index that asks for the callback, is the
# patched server as fast as master? (2) With one index that asks for it
# (dummy_index_am), what does fetching the old row and calling the index
# cost per row?
#
# master is build-bench (port 54330), the patch is build-bench-patch (port
# 54331); both -O2 without asserts, same settings, own clusters in data-f.
# Rounds alternate between the two so drift hits both.
#
# Default: four pgbench workloads on a 1M-row table. The bulk ones roll
# back and repeat on the same table, so later runs also pay for checking
# the aborted deletes.
# --fresh: one committed DELETE of 1M rows on a freshly built table
# each time, which shows the callback's per-row price.
#
# Usage: bench_delete.sh OUTDIR ROUNDS [--fresh]
#   SECS: seconds per one-row workload (default 10).
#   SQL: delete/*.sql.

source "$(dirname "$0")/lib.sh"
[ $# -ge 2 ] || usage
OUT=$1
ROUNDS=$2
MODE=${3:-}
CSV=$OUT/results.csv
SQL=$TOOLS/delete
WORKLOADS="bulk_delete bulk_update row_delete_insert row_update"

# on master|patch: point at that server.
on() {
	case $1 in
	master) use build-bench 54330 data-f ;;
	patch) use build-bench-patch 54331 data-f ;;
	esac
}

# create SERVER TABLE [notify]: a fresh 1M-row table; with notify, plus an
# index that asks for the callback.
create() {
	on "$1"
	export T=$2
	sql_file "$SQL/create.sql"
	[ "${3:-}" != notify ] || sql_file "$SQL/notify_index.sql"
	sql -c "VACUUM (ANALYZE) $T"
}

# workload ROUND SERVER TABLE WORKLOAD: one pgbench run of WORKLOAD.
workload() {
	local r=$1 s=$2 w=$4 script log
	on "$s"
	export T=$3
	script=$OUT/${w}_$T.sql
	log=$OUT/pgbench_${r}_${s}_${T}_$w.log
	render "$SQL/$w.sql" >"$script"
	sql -c "VACUUM $T" -c CHECKPOINT
	case $w in
	bulk_*) pgb "$log" -f "$script" -t 3 ;;
	*) pgb "$log" -f "$script" -T "$SECS" ;;
	esac
	echo "$r,$s,${w}_$T,$LAT,$TX" >>"$CSV"
	echo "  round $r $s ${w}_$T: $LAT ms ($TX)"
}

# fresh_delete ROUND SERVER [notify]: build the table, then time one DELETE of every row.
fresh_delete() {
	local r=$1 s=$2 name=bulk_delete_fresh_fd_plain ms
	[ "${3:-}" != notify ] || name=bulk_delete_fresh_fd_notify
	create "$s" fd_bulk "${3:-}"
	sql -c CHECKPOINT
	ms=$(sql -c '\timing on' -c 'DELETE FROM fd_bulk' | rg -o -m1 -r '$1' 'Time: ([0-9.]+) ms') ||
		die "no time from DELETE on $s"
	echo "$r,$s,$name,$ms,1" >>"$CSV"
	echo "  round $r $s $name: $ms ms"
}

SETTINGS="$BENCH_SETTINGS -c max_wal_size=8GB"
on master
up $SETTINGS
on patch
up $SETTINGS
mkdir -p "$OUT"
csv_header round,server,workload,latency_ms,transactions

if [ "$MODE" = --fresh ]; then
	for r in $(seq 1 "$ROUNDS"); do
		fresh_delete "$r" master
		fresh_delete "$r" patch
		fresh_delete "$r" patch notify
	done
else
	create master fd_plain
	create patch fd_plain
	create patch fd_notify notify
	for r in $(seq 1 "$ROUNDS"); do
		for w in $WORKLOADS; do
			workload "$r" master fd_plain "$w"
			workload "$r" patch fd_plain "$w"
			workload "$r" patch fd_notify "$w"
		done
	done
fi
echo done
