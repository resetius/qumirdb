# ClickBench benchmark queries

`run_clickbench.sh` runs the 43 ClickBench queries in [`sql/`](sql/) through
`qdb --sql`. The SQL is based on the canonical DuckDB query pack, with raw
integer date/time expressions adapted for qdb and quoted CamelCase identifiers
preserved for the Parquet schema.

The runner accepts either the directory containing `hits.parquet` or its parent
containing `pq1/hits.parquet`:

```sh
./run_clickbench.sh /path/to/clickbench
./run_clickbench.sh /path/to/clickbench/pq1 0,7,42

QDB_ARGS='--scheduler threaded --scheduler-workers 8' \
    ./run_clickbench.sh /path/to/clickbench
```

Queries q05, q21, q22, q27, and q28 are skipped by default because they require
string operations not currently supported by qdb. Set `ALLOW_UNSUPPORTED=1` to
attempt them. Per-query timing prints to stdout; diagnostics go to
`$OUT_DIR/clickbench.log` and result rows to `$OUT_DIR/clickbench.results`.

Dataset preparation and the upstream workload are documented by
[ClickBench](https://github.com/ClickHouse/ClickBench).
