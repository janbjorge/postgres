#!/usr/bin/env bash
# E: run fuzz_read.sql over several seeds, and check that it catches planted
# bugs (NOTES.md P3).
#
# fuzz_read.sql models grain's writer, sealer and reader rules in SQL and
# compares every read with a plain GROUP BY; its header says what it
# answers. This runs it per seed and totals the results. With --mutants it
# first runs copies with one bug planted each, and fails unless every copy
# finds a difference.
#
# Usage: fuzz_read.sh OUTDIR [--mutants]
#   Cluster: build-ra (asserts on), port 54329; started and stopped if it isn't running.
#   SEEDS: "SEED:ROWS ..." (default below). OPS, READS: per seed (default 10000, 300).

source "$(dirname "$0")/lib.sh"
[ $# -ge 1 ] || usage
OUT=$1
MODE=${2:-}
LOG=$OUT/run.log
SEEDS=${SEEDS:-"0.11:20000 0.22:20000 0.33:20000 0.44:20000 0.55:20000 0.66:1000000"}
OPS=${OPS:-10000}
READS=${READS:-300}

# fuzz FILE SEED ROWS OPS READS: run FILE; its output goes to OUT/last.log.
# Sets the summary line's fields: reads rows_differ bad_reads buckets_checked
# reads_with_markers max_markers summary_rows_used reads_all_scanned.
fuzz() {
	local line
	sql -v seed="$2" -v rows="$3" -v ops="$4" -v reads="$5" -f "$1" >"$OUT/last.log" 2>&1
	line=$(rg -o -m1 -r '$1 $2 $3 $4 $5 $6 $7 $8' \
		'^\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)\s*$' \
		"$OUT/last.log") || die "no summary line in $OUT/last.log"
	read -r reads rows_differ bad_reads buckets_checked reads_with_markers max_markers \
		summary_rows_used reads_all_scanned <<<"$line"
}

# mutant NAME OLD NEW: fuzz_read.sql with OLD (which must be in it once)
# replaced by NEW. Records NAME in MISSED unless some read comes out wrong.
MISSED=""
mutant() {
	local copy=$OUT/mutant.sql
	OLD=$2 NEW=$3 perl -0777 -ne '
		my $n = () = /\Q$ENV{OLD}\E/g;
		die "its text is in fuzz_read.sql $n times, not once\n" if $n != 1;
		s/\Q$ENV{OLD}\E/$ENV{NEW}/;
		print' "$TOOLS/fuzz_read.sql" >"$copy" || die "mutant $1: bad OLD text"
	fuzz "$copy" 0.42 2000 2000 60
	echo "  mutant: $1: $bad_reads bad reads of $reads" | tee -a "$LOG"
	[ "$bad_reads" -gt 0 ] || MISSED="$MISSED; $1"
}

use build-ra 54329
export PGOPTIONS="-c client_min_messages=error"
up
mkdir -p "$OUT"
: >"$LOG"

if [ "$MODE" = --mutants ]; then
	mutant "delete doesn't mark the old row" \
		'  IF FOUND THEN PERFORM touch(r.ts, r.status); END IF;' \
		'  NULL;'
	mutant "update doesn't mark the old row" \
		$'  PERFORM touch(r.ts, r.status);\n  PERFORM touch(p_ts, p_status);' \
		'  PERFORM touch(p_ts, p_status);'
	mutant "update doesn't mark the new row" \
		$'  PERFORM touch(r.ts, r.status);\n  PERFORM touch(p_ts, p_status);' \
		'  PERFORM touch(r.ts, r.status);'
	mutant "writer compares with W one minute early" \
		"p_status = 'paid' AND p_ts < meta.w" \
		"p_status = 'paid' AND p_ts < meta.w - interval '1 minute'"
	mutant "sealer skips marked minutes" \
		'UNION SELECT bucket FROM mark;' \
		'UNION SELECT bucket FROM mark WHERE false;'
	mutant "reader ignores markers" \
		'NOT EXISTS (SELECT 1 FROM mark WHERE mark.bucket >= vb' \
		'NOT EXISTS (SELECT 1 FROM mark WHERE false AND mark.bucket >= vb'
	mutant "reader checks a marker only at a bucket's own start" \
		'mark.bucket >= vb AND mark.bucket < vb + width(vl)' \
		'mark.bucket = vb'
	mutant "reader uses a row at the window's edge" \
		'IF vb >= f AND vb + width(vl) <= tt AND' \
		'IF true AND'
	mutant "reader uses a day row for hourly output" \
		'(wd IS NULL OR width(vl) <= wd)' \
		'true'
	[ -z "$MISSED" ] || die "planted bugs not caught$MISSED"
fi

total_reads=0 total_differ=0 total_bad=0 total_buckets=0 total_with_markers=0
total_max_markers=0 total_used=0 total_all_scanned=0
for pair in $SEEDS; do
	seed=${pair%%:*}
	rows=${pair##*:}
	fuzz "$TOOLS/fuzz_read.sql" "$seed" "$rows" "$OPS" "$READS"
	{ echo "== seed $seed, $rows rows"; cat "$OUT/last.log"; } >>"$LOG"
	echo "  seed $seed, $rows rows: reads $reads, rows_differ $rows_differ, bad_reads $bad_reads," \
		"buckets_checked $buckets_checked, reads_with_markers $reads_with_markers," \
		"max_markers $max_markers, summary_rows_used $summary_rows_used, reads_all_scanned $reads_all_scanned"
	total_reads=$((total_reads + reads))
	total_differ=$((total_differ + rows_differ))
	total_bad=$((total_bad + bad_reads))
	total_buckets=$((total_buckets + buckets_checked))
	total_with_markers=$((total_with_markers + reads_with_markers))
	[ "$max_markers" -le "$total_max_markers" ] || total_max_markers=$max_markers
	total_used=$((total_used + summary_rows_used))
	total_all_scanned=$((total_all_scanned + reads_all_scanned))
done

echo "total: reads $total_reads, rows_differ $total_differ, bad_reads $total_bad," \
	"buckets_checked $total_buckets, reads_with_markers $total_with_markers," \
	"max_markers $total_max_markers, summary_rows_used $total_used," \
	"reads_all_scanned $total_all_scanned" | tee -a "$LOG"
[ "$total_differ" = 0 ] || die "MISMATCH: see $LOG"
