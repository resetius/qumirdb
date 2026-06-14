#!/usr/bin/env bash
# DuckDB reference for filter_string_aggregate_string.sh.
set -euo pipefail

INPUT_FILE="${1:-${INPUT_FILE:-$HOME/Projects/tpch/pq10/orders.parquet}}"
OUTPUT_FILE="${OUTPUT_FILE:-/dev/null}"
THREADS="${THREADS:-1}"
DUCKDB_BIN="${DUCKDB_BIN:-/opt/homebrew/bin/duckdb}"
RAW_OUTPUT="$(mktemp "${TMPDIR:-/tmp}/duckdb-string-filter.XXXXXX")"
trap 'rm -f "$RAW_OUTPUT"' EXIT

SQL="
PRAGMA threads=${THREADS};
COPY (
  SELECT o_clerk,
         count(*) AS cnt,
         sum(o_orderkey) AS sum_orderkey
  FROM read_parquet('${INPUT_FILE}')
  WHERE o_orderstatus = 'F'
  GROUP BY o_clerk
  ORDER BY o_clerk
) TO '${RAW_OUTPUT}' (FORMAT CSV, HEADER false);
"

echo "[duckdb/string-filter] input=$INPUT_FILE threads=$THREADS output=$OUTPUT_FILE" >&2

/usr/bin/time -p "$DUCKDB_BIN" -c "$SQL"
sed -e 's/^"//' -e 's/",/,/' "$RAW_OUTPUT" > "$OUTPUT_FILE"
