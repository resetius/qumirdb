#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QDB_BIN="${QDB_BIN:-"$(cd "$SCRIPT_DIR/../.." && pwd)/build/bin/qdb"}"
QUERIES_DIR="$SCRIPT_DIR/queries"
PARAMS_DIR="$SCRIPT_DIR/params"

usage() {
    echo "Usage: $0 <base_dir> [scale] [query_filter]"
    echo "  base_dir      — directory containing pq1/, pq10/, pq100/ subdirs with parquet files"
    echo "  scale         — scale factor number: 1, 10, or 100 (default: all discovered)"
    echo "  query_filter  — optional comma-separated list of query numbers to run (e.g. 1,3,7)"
    exit 1
}

[[ $# -lt 1 ]] && usage
BASE_DIR="$(cd "$1" && pwd)"
SCALE_FILTER="${2:-}"
QUERY_FILTER="${3:-}"

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

# Run qdb and return "<seconds_float> <rc>" parsed from its output line:
#   "Processed N rowsets in X.XXXXXX seconds"
run_query() {
    local sexp_file="$1"
    local qdb_bin="$2"
    local out rc seconds
    set +e
    out=$("$qdb_bin" -i "$sexp_file" 2>&1)
    rc=$?
    set -e
    seconds=$(echo "$out" | grep -o 'Processed [0-9]* rowsets in [0-9.]*' | grep -o '[0-9.]*$' || echo "0")
    echo "$seconds $rc"
}

TMPDIR_BASE=$(mktemp -d)
trap 'rm -rf "$TMPDIR_BASE"' EXIT

for scale in "${SCALES[@]}"; do
    sf_num="${scale#pq}"
    params_file="$PARAMS_DIR/sf${sf_num}.sh"
    data_dir="$BASE_DIR/$scale"

    if [[ ! -f "$params_file" ]]; then
        echo "[SKIP] $scale — no params file $params_file"
        continue
    fi

    # shellcheck source=/dev/null
    source "$params_file"

    echo ""
    echo "========== Scale factor $sf_num ($scale) =========="
    printf "%-6s  %-12s  %s\n" "Query" "Time(s)" "Status"
    printf "%-6s  %-12s  %s\n" "------" "------------" "------"

    total_s="0"
    failed=0

    for q_num in $(seq 1 22); do
        query_enabled "$q_num" || continue

        template="$QUERIES_DIR/q${q_num}.sexp"
        [[ -f "$template" ]] || continue

        tmp_sexp="$TMPDIR_BASE/q${q_num}_sf${sf_num}.sexp"
        sed \
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
            "$template" > "$tmp_sexp"

        read -r seconds rc <<< "$(run_query "$tmp_sexp" "$QDB_BIN")"
        total_s=$(python3 -c "print(round($total_s + $seconds, 3))")

        if [[ $rc -eq 0 ]]; then
            printf "Q%-5s  %-12s  OK\n" "$q_num" "${seconds}s"
        else
            printf "Q%-5s  %-12s  FAILED\n" "$q_num" "${seconds}s"
            (( failed++ )) || true
        fi
    done

    echo "------"
    printf "Total: %ss,  Failed: %s\n" "$total_s" "$failed"
done
