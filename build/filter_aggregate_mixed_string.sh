#!/usr/bin/env bash
# Filter -> aggregate with a composite {i32, string} key.
# SQL equivalent:
#   SELECT o_custkey, o_orderstatus, count(*) AS cnt,
#          sum(o_orderkey) AS sum_orderkey
#   FROM orders
#   WHERE o_totalprice > 400000.0
#   GROUP BY o_custkey, o_orderstatus
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

INPUT_FILE="${1:-${INPUT_FILE:-$HOME/Projects/tpch/pq10/orders.parquet}}"
FORMAT="${FORMAT:-csv}"
OUTPUT_FILE="${OUTPUT_FILE:-/dev/null}"

echo "[qdb/mixed-string] input=$INPUT_FILE format=$FORMAT output=$OUTPUT_FILE" >&2

QUERY_FILE="$(mktemp "${TMPDIR:-/tmp}/qdb-mixed-string-query.XXXXXX")"
TIME_FILE="$(mktemp "${TMPDIR:-/tmp}/qdb-mixed-string-time.XXXXXX")"
cleanup() {
  status=$?
  if (( status != 0 )); then
    cat "$TIME_FILE" >&2
  fi
  rm -f "$QUERY_FILE" "$TIME_FILE"
  exit "$status"
}
trap cleanup EXIT

cat > "$QUERY_FILE" << EOF
(rel aggregate
  (rel filter
    (rel source "$INPUT_FILE")
    (: (> o_totalprice (: 400000.0 f64)) u8))
  (keys o_custkey o_orderstatus)
  (agg cnt count)
  (agg sum_orderkey sum o_orderkey))
EOF

CMD=(
  "$SCRIPT_DIR/bin/qdb"
  -i "$QUERY_FILE"
  --format "$FORMAT"
)

# Drop the CSV header and sort by both keys because hash-table order is
# unspecified, making the output diffable with the DuckDB reference.
/usr/bin/time -p "${CMD[@]}" 2>"$TIME_FILE" \
  | awk 'NR>1{rows++; print} END{printf("[qdb/mixed-string] rows=%d\n",rows)>"/dev/stderr"}' \
  | LC_ALL=C sort -t, -k1,1n -k2,2 \
  > "$OUTPUT_FILE"

cat "$TIME_FILE" >&2
