#!/usr/bin/env bash
# Filter -> aggregate with a string key.
# SQL equivalent:
#   SELECT o_orderstatus, count(*) AS cnt,
#          sum(o_orderkey) AS sum_orderkey,
#          min(o_orderkey) AS min_orderkey,
#          max(o_orderkey) AS max_orderkey
#   FROM orders
#   WHERE o_totalprice > 400000.0
#   GROUP BY o_orderstatus
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

INPUT_FILE="${1:-${INPUT_FILE:-$HOME/Projects/tpch/pq100/orders.parquet}}"
FORMAT="${FORMAT:-csv}"
OUTPUT_FILE="${OUTPUT_FILE:-/dev/null}"

echo "[qdb] input=$INPUT_FILE format=$FORMAT" >&2

QUERY_FILE="$(mktemp "${TMPDIR:-/tmp}/qdb-query.XXXXXX")"
TIME_FILE="$(mktemp "${TMPDIR:-/tmp}/qdb-time.XXXXXX")"
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
  (keys o_orderstatus)
  (agg cnt count)
  (agg sum_orderkey sum o_orderkey)
  (agg min_orderkey min o_orderkey)
  (agg max_orderkey max o_orderkey))
EOF

CMD=(
  "$SCRIPT_DIR/bin/qdb"
  -i "$QUERY_FILE"
  --format "$FORMAT"
)

# Drop the CSV header and sort by o_orderstatus because hash-table order is
# unspecified, making the output diffable with the DuckDB reference.
/usr/bin/time -p "${CMD[@]}" 2>"$TIME_FILE" \
  | awk 'NR>1{rows++; print} END{printf("[qdb] rows=%d\n",rows)>"/dev/stderr"}' \
  | LC_ALL=C sort -t, -k1,1 \
  > "$OUTPUT_FILE"

cat "$TIME_FILE" >&2
