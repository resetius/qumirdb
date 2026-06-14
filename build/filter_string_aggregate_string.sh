#!/usr/bin/env bash
# Filter by a string literal and aggregate by a string key.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

INPUT_FILE="${1:-${INPUT_FILE:-$HOME/Projects/tpch/pq10/orders.parquet}}"
FORMAT="${FORMAT:-csv}"
OUTPUT_FILE="${OUTPUT_FILE:-/dev/null}"

echo "[qdb/string-filter] input=$INPUT_FILE format=$FORMAT output=$OUTPUT_FILE" >&2

QUERY_FILE="$(mktemp "${TMPDIR:-/tmp}/qdb-string-filter-query.XXXXXX")"
TIME_FILE="$(mktemp "${TMPDIR:-/tmp}/qdb-string-filter-time.XXXXXX")"
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
    (: (== o_orderstatus "F") u8))
  (keys o_clerk)
  (agg cnt count)
  (agg sum_orderkey sum o_orderkey))
EOF

CMD=(
  "$SCRIPT_DIR/bin/qdb"
  -i "$QUERY_FILE"
  --format "$FORMAT"
)

/usr/bin/time -p "${CMD[@]}" 2>"$TIME_FILE" \
  | awk 'NR>1{rows++; print} END{printf("[qdb/string-filter] rows=%d\n",rows)>"/dev/stderr"}' \
  | LC_ALL=C sort -t, -k1,1 \
  > "$OUTPUT_FILE"

cat "$TIME_FILE" >&2
