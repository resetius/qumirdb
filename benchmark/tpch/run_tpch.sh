#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QDB_BIN="${QDB_BIN:-"$(cd "$SCRIPT_DIR/../.." && pwd)/build/bin/qdb"}"
SEXPR_DIR="$SCRIPT_DIR/queries/sexpr"
SQL_DIR="$SCRIPT_DIR/queries/sql"
PARAMS_DIR="$SCRIPT_DIR/params"
OUT_DIR="${OUT_DIR:-./results}"
MODE="${MODE:-sexpr}"
QDB_ARGS="${QDB_ARGS:-}"

usage() {
    echo "Usage: $0 <base_dir> [scale] [query_filter]"
    echo "  base_dir      — directory containing pq1/, pq10/, pq100/ subdirs with parquet files"
    echo "  scale         — scale factor number: 1, 10, or 100 (default: all discovered)"
    echo "  query_filter  — optional comma-separated list of query numbers to run (e.g. 1,3,7)"
    echo "Env:"
    echo "  MODE          — sexpr (default, runs queries/sexpr/*.sexp) or sql (runs queries/sql/*.sql via --sql)"
    echo "  QDB_ARGS      — extra arguments passed to qdb, e.g. '--scheduler threaded --scheduler-workers 4'"
    exit 1
}

[[ $# -lt 1 ]] && usage
[[ "$MODE" != "sexpr" && "$MODE" != "sql" ]] && { echo "Unknown MODE: $MODE" >&2; usage; }
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
    case ",$QUERY_FILTER," in *,"$n",*) return 0;; esac
    return 1
}

# Run qdb with the given args; append full output to LOG_FILE, return
# "<seconds_float> <rc>" on stdout.
run_query() {
    local label="$1"; shift
    local out rc seconds
    set +e
    out=$("$QDB_BIN" "$@" 2>&1)
    rc=$?
    set -e
    printf '\n=== %s (rc=%s) ===\n%s\n' "$label" "$rc" "$out" >> "$LOG_FILE"
    seconds=$(echo "$out" | grep -o 'Processed [0-9]* rowsets in [0-9.]*' | grep -o '[0-9.]*$' || echo "0")
    echo "$seconds $rc"
}

# Substitute scale params (and, for sexpr, table paths) into a template (stdout).
# sexpr templates use day-int dates (__Q3_DATE__); SQL templates use date-string
# placeholders (__Q3_DATE_S__) — both sets are substituted here.
render() {
    local template="$1" data_dir="$2"
    sed \
        -e "s|__Q1_DELTA__|${Q1_DELTA:-0}|g" \
        -e "s|__Q3_DATE_S__|${Q3_DATE_S:-}|g" \
        -e "s|__Q4_DATE_S__|${Q4_DATE_S:-}|g" \
        -e "s|__Q5_DATE_S__|${Q5_DATE_S:-}|g" \
        -e "s|__Q6_DATE_S__|${Q6_DATE_S:-}|g" \
        -e "s|__Q10_DATE_S__|${Q10_DATE_S:-}|g" \
        -e "s|__Q12_DATE_S__|${Q12_DATE_S:-}|g" \
        -e "s|__Q14_DATE_S__|${Q14_DATE_S:-}|g" \
        -e "s|__Q15_DATE_S__|${Q15_DATE_S:-}|g" \
        -e "s|__LINEITEM__|${data_dir}/lineitem.parquet|g" \
        -e "s|__ORDERS__|${data_dir}/orders.parquet|g" \
        -e "s|__CUSTOMER__|${data_dir}/customer.parquet|g" \
        -e "s|__SUPPLIER__|${data_dir}/supplier.parquet|g" \
        -e "s|__PART__|${data_dir}/part.parquet|g" \
        -e "s|__PARTSUPP__|${data_dir}/partsupp.parquet|g" \
        -e "s|__NATION__|${data_dir}/nation.parquet|g" \
        -e "s|__REGION__|${data_dir}/region.parquet|g" \
        -e "s|__Q1_SHIPDATE_MAX__|${Q1_SHIPDATE_MAX:-0}|g" \
        -e "s|__Q2_PART_SIZE__|${Q2_PART_SIZE:-0}|g" \
        -e "s|__Q2_REGION__|${Q2_REGION:-}|g" \
        -e "s|__Q3_SEGMENT__|${Q3_SEGMENT:-}|g" \
        -e "s|__Q3_DATE__|${Q3_DATE:-0}|g" \
        -e "s|__Q4_DATE_LOW__|${Q4_DATE_LOW:-0}|g" \
        -e "s|__Q4_DATE_HIGH__|${Q4_DATE_HIGH:-0}|g" \
        -e "s|__Q5_DATE_LOW__|${Q5_DATE_LOW:-0}|g" \
        -e "s|__Q5_DATE_HIGH__|${Q5_DATE_HIGH:-0}|g" \
        -e "s|__Q5_REGION__|${Q5_REGION:-}|g" \
        -e "s|__Q6_DATE_LOW__|${Q6_DATE_LOW:-0}|g" \
        -e "s|__Q6_DATE_HIGH__|${Q6_DATE_HIGH:-0}|g" \
        -e "s|__Q6_DISC_LOW__|${Q6_DISC_LOW:-0.0}|g" \
        -e "s|__Q6_DISC_HIGH__|${Q6_DISC_HIGH:-0.0}|g" \
        -e "s|__Q6_QUANTITY__|${Q6_QUANTITY:-0.0}|g" \
        -e "s|__Q7_NATION1__|${Q7_NATION1:-}|g" \
        -e "s|__Q7_NATION2__|${Q7_NATION2:-}|g" \
        -e "s|__Q8_DATE_LOW__|${Q8_DATE_LOW:-0}|g" \
        -e "s|__Q8_DATE_HIGH__|${Q8_DATE_HIGH:-0}|g" \
        -e "s|__Q8_REGION__|${Q8_REGION:-}|g" \
        -e "s|__Q8_PTYPE__|${Q8_PTYPE:-}|g" \
        -e "s|__Q8_NATION__|${Q8_NATION:-}|g" \
        -e "s|__Q10_DATE_LOW__|${Q10_DATE_LOW:-0}|g" \
        -e "s|__Q10_DATE_HIGH__|${Q10_DATE_HIGH:-0}|g" \
        -e "s|__Q11_NATION__|${Q11_NATION:-}|g" \
        -e "s|__Q12_SHIPMODE1__|${Q12_SHIPMODE1:-}|g" \
        -e "s|__Q12_SHIPMODE2__|${Q12_SHIPMODE2:-}|g" \
        -e "s|__Q12_DATE_LOW__|${Q12_DATE_LOW:-0}|g" \
        -e "s|__Q12_DATE_HIGH__|${Q12_DATE_HIGH:-0}|g" \
        -e "s|__Q14_DATE_LOW__|${Q14_DATE_LOW:-0}|g" \
        -e "s|__Q14_DATE_HIGH__|${Q14_DATE_HIGH:-0}|g" \
        -e "s|__Q15_DATE_LOW__|${Q15_DATE_LOW:-0}|g" \
        -e "s|__Q15_DATE_HIGH__|${Q15_DATE_HIGH:-0}|g" \
        -e "s|__Q16_BRAND__|${Q16_BRAND:-}|g" \
        -e "s|__Q17_BRAND__|${Q17_BRAND:-}|g" \
        -e "s|__Q17_CONTAINER__|${Q17_CONTAINER:-}|g" \
        -e "s|__Q20_NATION__|${Q20_NATION:-}|g" \
        -e "s|__Q20_DATE_LOW__|${Q20_DATE_LOW:-0}|g" \
        -e "s|__Q20_DATE_HIGH__|${Q20_DATE_HIGH:-0}|g" \
        -e "s|__Q21_NATION__|${Q21_NATION:-}|g" \
        -e "s|__Q22_CC1__|${Q22_CC1:-}|g" \
        -e "s|__Q22_CC2__|${Q22_CC2:-}|g" \
        -e "s|__Q22_CC3__|${Q22_CC3:-}|g" \
        -e "s|__Q22_CC4__|${Q22_CC4:-}|g" \
        -e "s|__Q22_CC5__|${Q22_CC5:-}|g" \
        -e "s|__Q22_CC6__|${Q22_CC6:-}|g" \
        -e "s|__Q22_CC7__|${Q22_CC7:-}|g" \
        "$template"
}

mkdir -p "$OUT_DIR"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

for scale in "${SCALES[@]}"; do
    sf_num="${scale#pq}"
    params_file="$PARAMS_DIR/sf${sf_num}.sh"
    data_dir="$BASE_DIR/$scale"

    # Scale params drive placeholder substitution for both modes.
    if [[ ! -f "$params_file" ]]; then
        echo "[qdb] skip $scale — no params file $params_file" >&2
        continue
    fi
    # shellcheck source=/dev/null
    source "$params_file"

    LOG_FILE="$OUT_DIR/tpch_${MODE}_sf${sf_num}.log"
    : > "$LOG_FILE"

    echo "[qdb] mode=$MODE scale=$sf_num parquet=$data_dir" | tee -a "$LOG_FILE" >&2

    printf "%-6s  %-12s  %s\n" "Query" "Time(s)" "Status"
    printf "%-6s  %-12s  %s\n" "------" "------------" "------"

    total_s="0"
    failed=0

    for q_num in $(seq 1 22); do
        query_enabled "$q_num" || continue

        if [[ "$MODE" == "sql" ]]; then
            template="$SQL_DIR/q${q_num}.sql"
            [[ -f "$template" ]] || continue
            tmp_sql="$tmpdir/q${q_num}_sf${sf_num}.sql"
            render "$template" "$data_dir" > "$tmp_sql"
            read -r seconds rc <<< "$(run_query "Q${q_num} sf${sf_num}" "${EXTRA_QDB_ARGS[@]}" --sql -i "$tmp_sql" --data "$data_dir")"
        else
            template="$SEXPR_DIR/q${q_num}.sexp"
            [[ -f "$template" ]] || continue
            tmp_sexp="$tmpdir/q${q_num}_sf${sf_num}.sexp"
            render "$template" "$data_dir" > "$tmp_sexp"
            read -r seconds rc <<< "$(run_query "Q${q_num} sf${sf_num}" "${EXTRA_QDB_ARGS[@]}" -i "$tmp_sexp")"
        fi

        total_s=$(python3 -c "print(round($total_s + $seconds, 3))")

        if [[ $rc -eq 0 ]]; then
            printf "Q%-5s  %-12s  OK\n" "$q_num" "${seconds}s"
            echo "[qdb] q${q_num} ${seconds}s OK" >> "$LOG_FILE"
        else
            printf "Q%-5s  %-12s  FAILED\n" "$q_num" "${seconds}s"
            echo "[qdb] q${q_num} FAILED" >> "$LOG_FILE"
            (( failed++ )) || true
        fi
    done

    printf "Total: %ss,  Failed: %s\n" "$total_s" "$failed"
    echo "[qdb] log: $LOG_FILE" >&2
done
