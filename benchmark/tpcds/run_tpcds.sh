#!/usr/bin/env bash
set -euo pipefail

# TPC-DS runner. Unlike the TPC-H harness this does no templating yet: the query
# files carry concrete literals and are run as-is. Tables are referenced by bare
# name and resolved by qdb's --data <dir> to <dir>/<table>.parquet.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QDB_BIN="${QDB_BIN:-"$(cd "$SCRIPT_DIR/../.." && pwd)/build/bin/qdb"}"
SQL_DIR="$SCRIPT_DIR/queries/sql"
OUT_DIR="${OUT_DIR:-./results}"
QDB_ARGS="${QDB_ARGS:-}"

usage() {
    echo "Usage: $0 <base_dir> [scale] [query_filter]"
    echo "  base_dir      — directory containing pq1/, pq10/, pq100/ subdirs with parquet files"
    echo "  scale         — scale factor number: 1, 10, or 100 (default: all discovered)"
    echo "  query_filter  — optional comma-separated list of query numbers to run (e.g. 6,19,42)"
    echo "Env:"
    echo "  QDB_ARGS      — extra arguments passed to qdb, e.g. '--scheduler threaded --scheduler-workers 4'"
    exit 1
}

[[ $# -lt 1 ]] && usage
BASE_DIR="$(cd "$1" && pwd)"
SCALE_FILTER="${2:-}"
QUERY_FILTER="${3:-}"
read -r -a EXTRA_QDB_ARGS <<< "$QDB_ARGS"

# Discover scale subdirectories (pq<N>)
ALL_SCALES=()
for d in "$BASE_DIR"/pq*/; do
    [[ -d "$d" ]] && ALL_SCALES+=("$(basename "$d")")
done

if [[ ${#ALL_SCALES[@]} -eq 0 ]]; then
    echo "No pq* subdirectories found under $BASE_DIR" >&2
    exit 1
fi

# Filter to requested scale if given
SCALES=()
if [[ -n "$SCALE_FILTER" ]]; then
    target="pq${SCALE_FILTER}"
    for s in "${ALL_SCALES[@]}"; do
        [[ "$s" == "$target" ]] && SCALES+=("$s")
    done
    if [[ ${#SCALES[@]} -eq 0 ]]; then
        echo "Scale pq${SCALE_FILTER} not found under $BASE_DIR" >&2
        exit 1
    fi
else
    SCALES=("${ALL_SCALES[@]}")
fi

query_enabled() {
    local n="$1"
    [[ -z "$QUERY_FILTER" ]] && return 0
    # match with and without leading zeros (6 == 06)
    case ",$QUERY_FILTER," in
        *,"$n",*) return 0;;
        *,"$((10#$n))",*) return 0;;
    esac
    return 1
}

# Run qdb, splitting the two streams: result rows (stdout) -> RESULT_FILE, and
# diagnostics/timing/errors (stderr) plus the OK/FAILED status -> LOG_FILE. Echoes
# "<seconds_float> <rc>" on stdout. qdb prints "Returned N rows in X seconds" and any
# error to stderr, so the timing is parsed from the stderr stream.
run_query() {
    local label="$1"; shift
    local out_file err_file rc seconds
    out_file="$(mktemp "$tmpdir/qdb_output.XXXXXX")"
    err_file="$(mktemp "$tmpdir/qdb_stderr.XXXXXX")"
    set +e
    "$QDB_BIN" "$@" > "$out_file" 2> "$err_file"
    rc=$?
    set -e
    printf '\n=== %s (rc=%s) ===\n' "$label" "$rc" >> "$RESULT_FILE"
    cat "$out_file" >> "$RESULT_FILE"
    printf '\n=== %s (rc=%s) ===\n' "$label" "$rc" >> "$LOG_FILE"
    cat "$err_file" >> "$LOG_FILE"
    printf '\n' >> "$LOG_FILE"
    seconds=$(
        sed -nE \
            -e 's/.*Returned [0-9]+ rows in ([0-9]+([.][0-9]+)?) seconds.*/\1/p' \
            -e 's/.*Processed [0-9]+ rowsets in ([0-9]+([.][0-9]+)?).*/\1/p' \
            "$err_file" | tail -n 1
    )
    seconds="${seconds:-0}"
    echo "$seconds $rc"
}

mkdir -p "$OUT_DIR"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

for scale in "${SCALES[@]}"; do
    sf_num="${scale#pq}"
    data_dir="$BASE_DIR/$scale"

    LOG_FILE="$OUT_DIR/tpcds_sql_sf${sf_num}.log"       # status + errors (analyze this)
    RESULT_FILE="$OUT_DIR/tpcds_sql_sf${sf_num}.results" # query result rows
    : > "$LOG_FILE"
    : > "$RESULT_FILE"

    echo "[qdb] tpcds scale=$sf_num parquet=$data_dir" | tee -a "$LOG_FILE" >&2

    printf "%-6s  %-12s  %s\n" "Query" "Time(s)" "Status"
    printf "%-6s  %-12s  %s\n" "------" "------------" "------"

    total_s="0"
    failed=0

    for template in "$SQL_DIR"/q*.sql; do
        [[ -f "$template" ]] || continue
        base="$(basename "$template" .sql)"  # e.g. q06
        q_num="${base#q}"
        query_enabled "$q_num" || continue

        read -r seconds rc <<< "$(run_query "${base} sf${sf_num}" \
            ${EXTRA_QDB_ARGS[@]+"${EXTRA_QDB_ARGS[@]}"} --sql -i "$template" --data "$data_dir")"

        total_s=$(python3 -c "print(round($total_s + $seconds, 3))")

        if [[ $rc -eq 0 ]]; then
            printf "%-6s  %-12s  OK\n" "$base" "${seconds}s"
            echo "[qdb] ${base} ${seconds}s OK" >> "$LOG_FILE"
        else
            printf "%-6s  %-12s  FAILED\n" "$base" "${seconds}s"
            echo "[qdb] ${base} FAILED" >> "$LOG_FILE"
            (( failed++ )) || true
        fi
    done

    printf "Total: %ss,  Failed: %s\n" "$total_s" "$failed"
    echo "[qdb] log: $LOG_FILE  results: $RESULT_FILE" >&2
done
