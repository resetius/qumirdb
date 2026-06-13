#!/usr/bin/env bash
# DuckDB reference for filter_aggregate_mixed_string.sh.
set -euo pipefail

INPUT_FILE="${1:-${INPUT_FILE:-$HOME/Projects/tpch/pq10/orders.parquet}}"
OUTPUT_FILE="${OUTPUT_FILE:-/dev/null}"
THREADS="${THREADS:-1}"
DUCKDB_BIN="${DUCKDB_BIN:-/opt/homebrew/bin/duckdb}"

SQL="
PRAGMA threads=${THREADS};
COPY (
  SELECT o_custkey,
         o_orderstatus,
         count(*) AS cnt,
         sum(o_orderkey) AS sum_orderkey
  FROM read_parquet('${INPUT_FILE}')
  WHERE o_totalprice > 400000.0
  GROUP BY o_custkey, o_orderstatus
  ORDER BY o_custkey, o_orderstatus
) TO '${OUTPUT_FILE}' (FORMAT CSV, HEADER false);
"

echo "[duckdb/mixed-string] input=$INPUT_FILE threads=$THREADS output=$OUTPUT_FILE" >&2

/usr/bin/time -p "$DUCKDB_BIN" -c "$SQL"
