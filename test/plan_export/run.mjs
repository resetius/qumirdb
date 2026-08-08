import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawnSync } from 'node:child_process';

const [exporter, nativeRunner, runtimePath, goldensDir] = process.argv.slice(2);
if (!exporter || !nativeRunner || !runtimePath || !goldensDir) {
  throw new Error(
    'usage: run.mjs <qdb_plan_export> <native_runner> <browser_runtime> <goldens>');
}

const { executeBrowserPipelineScheduled } = await import(
  pathToFileURL(runtimePath).href);

const fixtures = {
  filter_project: {
    sql: 'SELECT k AS k, v + 1 AS x FROM t WHERE v >= 20',
    tables: {
      t: { k: [1, 2, 3, 4], v: [10, 20, 30, 40] },
    },
  },
  aggregate: {
    sql: 'SELECT k AS k, sum(v) AS s FROM t GROUP BY k',
    tables: {
      t: { k: [1, 1, 2, 2, 2], v: [10, 20, 3, 4, 5] },
    },
  },
  join: {
    sql: 'SELECT l.k AS k, l.v + r.w AS total FROM l JOIN r ON l.k = r.rk',
    tables: {
      l: { k: [1, 2, 3, 4], v: [10, 20, 30, 40] },
      r: { rk: [2, 4, 5], w: [200, 400, 500] },
    },
  },
  nested_limit: {
    sql: 'SELECT count(*) AS n FROM '
      + '(SELECT k FROM t WHERE k >= 2 LIMIT 2 OFFSET 1) u',
    tables: {
      t: { k: [1, 2, 3, 4, 5] },
    },
    browserBatchSizes: { t: [2, 1, 2] },
  },
  nested_limit_zero: {
    sql: 'SELECT count(*) AS n FROM (SELECT k FROM t LIMIT 0) u',
    tables: {
      t: { k: [1, 2, 3] },
    },
    browserBatchSizes: { t: [1, 2] },
  },
};

function datasetFor(fixture) {
  return {
    tables: Object.entries(fixture.tables).map(([name, columns]) => {
      const rowCount = Object.values(columns)[0]?.length ?? 0;
      return {
        name,
        columns: Object.keys(columns).map(column => ({
          name: column,
          type: 'i64',
        })),
        stats: {
          rows: rowCount,
          bytes: rowCount * Object.keys(columns).length * 8,
          rowGroups: 4,
        },
      };
    }),
  };
}

function run(command, args, input = undefined) {
  const result = spawnSync(command, args, {
    input,
    encoding: 'utf8',
    maxBuffer: 128 * 1024 * 1024,
  });
  if (result.error) {
    throw result.error;
  }
  assert.equal(
    result.status,
    0,
    `${command} failed (${result.status}):\n${result.stderr}`);
  return result.stdout;
}

function exportBundle(name, mode, embedWasm) {
  const fixture = fixtures[name];
  const multi = mode === 'threaded';
  const request = {
    sql: fixture.sql,
    dataset: datasetFor(fixture),
    options: {
      scheduler: mode,
      schedulerWorkers: multi ? 4 : 1,
      scanTasks: multi ? 4 : 1,
      shufflePartitions: multi ? 2 : 1,
      format: 'runtime-bundle',
      embedWasm,
    },
  };
  const bundle = JSON.parse(run(
    exporter,
    ['--stdin-json', '--stdout-json'],
    JSON.stringify(request)));
  assert.equal(bundle.ok, true, `${name}/${mode}: exporter rejected request`);
  assert.equal(
    bundle.exec?.supported,
    true,
    `${name}/${mode}: ${bundle.exec?.reason || 'unsupported exec plan'}`);
  return bundle;
}

function normalizedExec(exec) {
  const normalized = {
    supported: exec.supported,
    embedWasm: exec.embedWasm,
    nodes: exec.nodes,
    edges: exec.edges,
    root: exec.root,
  };
  if (exec.limit !== undefined) {
    normalized.limit = exec.limit;
  }
  return normalized;
}

function assertStablePhysicalGroups(name, mode, bundle) {
  const physical = bundle.graphs?.physical;
  assert.ok(physical, `${name}/${mode}: physical graph is missing`);
  for (const node of physical.nodes) {
    assert.match(
      node.taskGroup,
      /^(exec|task):[1-9][0-9]*$/,
      `${name}/${mode}: unstable physical task group ${node.taskGroup}`);
  }
  assert.deepEqual(
    bundle.graph,
    physical,
    `${name}/${mode}: legacy physical graph mirror drifted`);
}

function resolveExecArtifacts(bundle) {
  const artifactId = bundle.exec.wasm;
  const wasm = bundle.artifacts?.[artifactId]?.data;
  assert.ok(wasm, `exec artifact is missing: ${artifactId}`);
  return { ...bundle.exec, wasm };
}

function readGolden(name, mode) {
  const file = path.join(goldensDir, `${name}.${mode}.json`);
  return JSON.parse(readFileSync(file, 'utf8'));
}

function nativeResult(name, mode) {
  return JSON.parse(run(nativeRunner, [name, mode]));
}

function normalizeResult(result) {
  const rows = result.rows.map(row => row.map(value => String(value)));
  rows.sort((left, right) =>
    JSON.stringify(left).localeCompare(JSON.stringify(right)));
  return {
    columns: result.columns.map(String),
    rows,
  };
}

async function browserResult(name, exec) {
  const fixture = fixtures[name];
  const readSourceBatches = async function* (stage) {
    const table = fixture.tables[stage.table];
    assert.ok(table, `${name}: browser requested unknown table ${stage.table}`);
    const rowCount = Object.values(table)[0]?.length ?? 0;
    const batchSizes = fixture.browserBatchSizes?.[stage.table] ?? [rowCount];
    assert.equal(
      batchSizes.reduce((sum, size) => sum + size, 0),
      rowCount,
      `${name}: browser batch sizes do not cover table ${stage.table}`);
    let begin = 0;
    for (const batchSize of batchSizes) {
      const end = begin + batchSize;
      yield {
        rowCount: batchSize,
        columns: stage.columns.map(column => {
          const values = table[column.name];
          assert.ok(
            values,
            `${name}: browser requested unknown column ${column.name}`);
          return { ...column, values: values.slice(begin, end) };
        }),
      };
      begin = end;
    }
  };
  return executeBrowserPipelineScheduled(exec, readSourceBatches);
}

const printGoldens = process.env.QDB_PRINT_GOLDENS === '1';

for (const name of Object.keys(fixtures)) {
  for (const mode of ['single', 'threaded']) {
    const snapshotBundle = exportBundle(name, mode, false);
    assertStablePhysicalGroups(name, mode, snapshotBundle);
    const snapshot = normalizedExec(snapshotBundle.exec);
    if (printGoldens) {
      console.log(`=== ${name}.${mode}.json ===`);
      console.log(JSON.stringify(snapshot, null, 2));
      continue;
    }
    assert.deepEqual(snapshot, readGolden(name, mode),
      `${name}/${mode}: exported exec JSON changed`);

    if (name === 'join' && mode === 'threaded') {
      const repeated = exportBundle(name, mode, false);
      assert.deepEqual(
        snapshotBundle.graphs.physical,
        repeated.graphs.physical,
        `${name}/${mode}: physical graph changed across exporter processes`);
    }

    const runtimeBundle = exportBundle(name, mode, true);
    const browser = await browserResult(
      name, resolveExecArtifacts(runtimeBundle));
    assert.equal(
      browser.memory?.liveMB,
      '0.0',
      `${name}/${mode}: browser runtime leaked wasm memory`);
    assert.deepEqual(
      normalizeResult(browser),
      normalizeResult(nativeResult(name, mode)),
      `${name}/${mode}: browser and native results differ`);
  }
}

if (!printGoldens) {
  console.log('plan export golden and native/browser parity checks passed');
}
