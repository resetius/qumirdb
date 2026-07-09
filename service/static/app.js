import { getJson, postJson } from './api.js';
import { renderGraph } from './graph.js';
import { tpchQueries } from './tpch_queries.js';
import {
  addFilesToBrowserDataset,
  createBrowserDataset,
  deleteBrowserDataset,
  listBrowserDatasets,
  removeFileFromBrowserDataset,
  renameBrowserDataset
} from './browser_storage.js';
import { readParquetTable } from './browser_parquet.js';

const $ = selector => document.querySelector(selector);

const QUERIES_KEY = 'qdb.web.queries';
const ACTIVE_QUERY_KEY = 'qdb.web.activeQuery';
const ACTIVE_DATASET_KEY = 'qdb.web.activeDataset';

let editor = null;
let activeQueryId = localStorage.getItem(ACTIVE_QUERY_KEY) || '';
let suppressEditorSave = false;
let activeDatasetId = localStorage.getItem(ACTIVE_DATASET_KEY) || '';
let serverDatasets = [];
let browserDatasets = [];
let lastBundle = null;
let lastExplainKey = null;
let graphMode = 'logical';
let resultRows = [];
let resultSort = null;
let inspectorSummaryText = '';
let inspectorArtifactItems = [];

window.addEventListener('DOMContentLoaded', () => {
  window.lucide?.createIcons();
  initEditor();
  initDrawers();
  initTabs();
  initGraphControls();
  initQueries();
  initDatasets();
  initActions();
});

function initEditor() {
  const textarea = $('#sql-editor');
  if (window.CodeMirror) {
    editor = window.CodeMirror.fromTextArea(textarea, {
      mode: 'text/x-sql',
      theme: 'material-darker',
      lineNumbers: true,
      matchBrackets: true
    });
    editor.setSize('100%', '100%');
    editor.on('change', saveActiveQuerySql);
  } else {
    textarea.addEventListener('input', saveActiveQuerySql);
  }
}

function getSql() {
  return editor ? editor.getValue() : $('#sql-editor').value;
}

function setSql(sql) {
  suppressEditorSave = true;
  if (editor) {
    editor.setValue(sql);
  } else {
    $('#sql-editor').value = sql;
  }
  suppressEditorSave = false;
}

function initDrawers() {
  $('#queries-toggle').addEventListener('click', () => {
    $('#queries-pane').classList.toggle('open');
  });
  $('#datasets-toggle').addEventListener('click', () => {
    $('#datasets-pane').classList.toggle('open');
  });
}

function initTabs() {
  for (const tab of document.querySelectorAll('.tab')) {
    tab.addEventListener('click', () => {
      for (const item of document.querySelectorAll('.tab')) {
        item.classList.toggle('active', item === tab);
      }
      for (const panel of document.querySelectorAll('.tab-panel')) {
        panel.classList.toggle('active', panel.id === `${tab.dataset.tab}-tab`);
      }
    });
  }
}

function initGraphControls() {
  for (const button of document.querySelectorAll('.graph-mode')) {
    button.addEventListener('click', () => {
      graphMode = button.dataset.mode || 'logical';
      for (const item of document.querySelectorAll('.graph-mode')) {
        item.classList.toggle('active', item === button);
      }
      renderCurrentGraph();
      clearInspector();
    });
  }
  $('#inspector-clear').addEventListener('click', clearInspector);
  $('#inspector-artifact-select').addEventListener('change', event => {
    renderSelectedInspectorArtifact(Number(event.target.value || 0));
  });
}

function initQueries() {
  let queries = ensureTpchQueries(loadQueries());
  if (!activeQueryId) {
    activeQueryId = 'tpch-q21';
  }
  if (!queries.some(query => query.id === activeQueryId)) {
    activeQueryId = queries[0]?.id || '';
  }
  saveQueries(queries);
  localStorage.setItem(ACTIVE_QUERY_KEY, activeQueryId);

  const activeQuery = queries.find(query => query.id === activeQueryId);
  if (activeQuery) {
    setSql(activeQuery.sql || '');
  }

  $('#query-new').addEventListener('click', () => {
    const queries = loadQueries();
    const query = {
      id: crypto.randomUUID(),
      name: `Query ${queries.length + 1}`,
      sql: ''
    };
    queries.unshift(query);
    activeQueryId = query.id;
    localStorage.setItem(ACTIVE_QUERY_KEY, activeQueryId);
    saveQueries(queries);
    setSql('');
    renderQueries();
  });
  renderQueries();
}

function ensureTpchQueries(queries) {
  const existing = new Set(queries.map(query => query.id));
  const missing = tpchQueries.filter(query => !existing.has(query.id));
  if (!missing.length) {
    return queries;
  }
  return [
    ...missing,
    ...queries
  ];
}

function loadQueries() {
  try {
    return JSON.parse(localStorage.getItem(QUERIES_KEY) || '[]');
  } catch {
    return [];
  }
}

function saveQueries(queries) {
  localStorage.setItem(QUERIES_KEY, JSON.stringify(queries));
}

function saveActiveQuerySql() {
  if (suppressEditorSave || !activeQueryId) {
    return;
  }
  const queries = loadQueries();
  const index = queries.findIndex(query => query.id === activeQueryId);
  if (index < 0) {
    return;
  }
  queries[index] = {
    ...queries[index],
    sql: getSql()
  };
  saveQueries(queries);
}

function renderQueries() {
  const root = $('#queries-list');
  root.replaceChildren();
  for (const query of loadQueries()) {
    const button = document.createElement('button');
    button.className = `list-item${query.id === activeQueryId ? ' active' : ''}`;
    button.type = 'button';
    button.textContent = query.name;
    button.addEventListener('click', () => {
      saveActiveQuerySql();
      activeQueryId = query.id;
      localStorage.setItem(ACTIVE_QUERY_KEY, activeQueryId);
      const freshQuery = loadQueries().find(item => item.id === activeQueryId);
      setSql(freshQuery?.sql || '');
      renderQueries();
    });
    root.appendChild(button);
  }
}

async function initDatasets() {
  serverDatasets = await loadServerDatasets();
  browserDatasets = await loadBrowserDatasets();
  ensureActiveDataset();

  $('#dataset-create').addEventListener('click', createEmptyBrowserDataset);
  $('#dataset-files').addEventListener('change', async event => {
    await addFilesToActiveBrowserDataset(Array.from(event.target.files || []));
    event.target.value = '';
  });
  renderDatasets();
}

async function loadBrowserDatasets() {
  try {
    return await listBrowserDatasets();
  } catch (error) {
    showDetails({
      ok: false,
      error: {
        stage: 'browser-storage',
        message: error.message || String(error)
      }
    });
    return [];
  }
}

async function loadServerDatasets() {
  const result = await getJson('/api/datasets');
  if (result.ok === false) {
    return [];
  }
  return result.datasets || [];
}

async function createEmptyBrowserDataset() {
  const name = window.prompt('Dataset name', `Dataset ${browserDatasets.length + 1}`);
  const trimmed = name?.trim();
  if (!trimmed) {
    return;
  }
  try {
    const dataset = await createBrowserDataset(trimmed);
    browserDatasets = await loadBrowserDatasets();
    activeDatasetId = dataset.id;
    localStorage.setItem(ACTIVE_DATASET_KEY, activeDatasetId);
    renderDatasets();
  } catch (error) {
    showDetails({
      ok: false,
      error: {
        stage: 'browser-storage',
        message: error.message || String(error)
      }
    });
    selectTab('details');
  }
}

function activeDataset() {
  return allDatasets().find(dataset => dataset.id === activeDatasetId) || null;
}

function allDatasets() {
  return [...serverDatasets, ...browserDatasets];
}

function ensureActiveDataset() {
  const datasets = allDatasets();
  if (!datasets.some(dataset => dataset.id === activeDatasetId)) {
    activeDatasetId = datasets[0]?.id || '';
  }
  if (activeDatasetId) {
    localStorage.setItem(ACTIVE_DATASET_KEY, activeDatasetId);
  } else {
    localStorage.removeItem(ACTIVE_DATASET_KEY);
  }
}

async function addFilesToActiveBrowserDataset(files) {
  const dataset = activeDataset();
  if (dataset?.source?.kind !== 'browser') {
    showDetails({
      ok: false,
      error: { message: 'Select a browser dataset before adding parquet files.' }
    });
    selectTab('details');
    return;
  }

  const parquetFiles = files.filter(file => file.name.toLowerCase().endsWith('.parquet'));
  if (!parquetFiles.length) {
    showDetails({ ok: false, error: { message: 'Select one or more .parquet files.' } });
    selectTab('details');
    return;
  }

  setStatus('reading parquet');
  try {
    const tables = [];
    for (const file of parquetFiles) {
      tables.push({
        ...(await readParquetTable(file)),
        sourceFile: file.name
      });
    }
    await addFilesToBrowserDataset(dataset, parquetFiles, tables);
    browserDatasets = await loadBrowserDatasets();
    ensureActiveDataset();
    renderDatasets();
    setStatus('dataset ready');
  } catch (error) {
    setStatus('dataset failed');
    showDetails({ ok: false, error: { message: error.message || String(error) } });
    selectTab('details');
  }
}

async function renameActiveBrowserDataset(dataset) {
  const name = window.prompt('Dataset name', dataset.name || '');
  const trimmed = name?.trim();
  if (!trimmed || trimmed === dataset.name) {
    return;
  }
  try {
    await renameBrowserDataset(dataset, trimmed);
    browserDatasets = await loadBrowserDatasets();
    renderDatasets();
  } catch (error) {
    showDetails({
      ok: false,
      error: {
        stage: 'browser-storage',
        message: error.message || String(error)
      }
    });
    selectTab('details');
  }
}

function renderDatasets() {
  const root = $('#datasets-list');
  root.replaceChildren();
  for (const dataset of allDatasets()) {
    const row = document.createElement('div');
    row.className = `dataset-item${dataset.id === activeDatasetId ? ' active' : ''}`;
    const button = document.createElement('button');
    button.className = 'dataset-select';
    button.type = 'button';
    button.textContent = dataset.source?.kind === 'server'
      ? `${dataset.name} · server`
      : `${dataset.name} · browser`;
    button.addEventListener('click', () => {
      activeDatasetId = dataset.id;
      localStorage.setItem(ACTIVE_DATASET_KEY, activeDatasetId);
      renderDatasets();
    });
    row.appendChild(button);
    if (dataset.source?.kind === 'browser') {
      const upload = document.createElement('button');
      upload.className = 'icon-button dataset-action';
      upload.type = 'button';
      upload.title = 'Add parquet files';
      upload.setAttribute('aria-label', 'Add parquet files');
      upload.innerHTML = '<i data-lucide="upload"></i>';
      upload.addEventListener('click', event => {
        event.stopPropagation();
        activeDatasetId = dataset.id;
        localStorage.setItem(ACTIVE_DATASET_KEY, activeDatasetId);
        $('#dataset-files').click();
      });
      row.appendChild(upload);

      const rename = document.createElement('button');
      rename.className = 'icon-button dataset-action';
      rename.type = 'button';
      rename.title = 'Rename dataset';
      rename.setAttribute('aria-label', 'Rename dataset');
      rename.innerHTML = '<i data-lucide="pencil"></i>';
      rename.addEventListener('click', async event => {
        event.stopPropagation();
        await renameActiveBrowserDataset(dataset);
      });
      row.appendChild(rename);

      const remove = document.createElement('button');
      remove.className = 'icon-button dataset-action';
      remove.type = 'button';
      remove.title = 'Delete dataset';
      remove.setAttribute('aria-label', 'Delete dataset');
      remove.innerHTML = '<i data-lucide="trash-2"></i>';
      remove.addEventListener('click', async event => {
        event.stopPropagation();
        await deleteBrowserDataset(dataset.id);
        browserDatasets = await loadBrowserDatasets();
        ensureActiveDataset();
        renderDatasets();
      });
      row.appendChild(remove);
    }
    root.appendChild(row);
  }
  window.lucide?.createIcons();
  renderSchema(activeDataset());
}

function renderSchema(dataset) {
  const root = $('#schema-tree');
  root.replaceChildren();
  if (!dataset) {
    const empty = document.createElement('div');
    empty.className = 'schema-empty';
    empty.textContent = 'No dataset selected.';
    root.appendChild(empty);
    return;
  }
  if (!(dataset.tables || []).length) {
    const empty = document.createElement('div');
    empty.className = 'schema-empty';
    empty.textContent = dataset.source?.kind === 'browser'
      ? 'No parquet files in this dataset.'
      : 'No tables.';
    root.appendChild(empty);
    return;
  }
  for (const item of dataset.tables || []) {
    const tableRoot = document.createElement('div');
    tableRoot.className = 'schema-table';
    const header = document.createElement('div');
    header.className = 'schema-table-header';
    const title = document.createElement('div');
    title.className = 'schema-table-name';
    title.textContent = item.stats
      ? `${item.name} · ${formatNumber(item.stats.rows || 0)} rows · ${item.stats.rowGroups || 1} rg`
      : item.name;
    header.appendChild(title);
    if (dataset.source?.kind === 'browser') {
      const sourceFile = browserTableSourceFile(dataset, item);
      const remove = document.createElement('button');
      remove.className = 'icon-button schema-table-action';
      remove.type = 'button';
      remove.title = 'Remove parquet file';
      remove.setAttribute('aria-label', 'Remove parquet file');
      remove.innerHTML = '<i data-lucide="trash-2"></i>';
      remove.addEventListener('click', async () => {
        await removeBrowserDatasetFile(dataset, sourceFile);
      });
      header.appendChild(remove);
    }
    tableRoot.appendChild(header);
    for (const column of item.columns || []) {
      const row = document.createElement('div');
      row.className = 'schema-column';
      row.innerHTML = `<span>${escapeHtml(column.name)}</span><span>${escapeHtml(column.type)}</span>`;
      tableRoot.appendChild(row);
    }
    root.appendChild(tableRoot);
  }
  window.lucide?.createIcons();
}

function browserTableSourceFile(dataset, table) {
  if (table.sourceFile) {
    return table.sourceFile;
  }
  const files = dataset.source?.files || [];
  return files.find(file => tableNameFromFile(file.name) === table.name)?.name ||
    `${table.name}.parquet`;
}

function tableNameFromFile(fileName) {
  return fileName
    .replace(/[.]parquet$/i, '')
    .replace(/[^A-Za-z0-9_]+/g, '_')
    .replace(/^_+|_+$/g, '') || 'table';
}

async function removeBrowserDatasetFile(dataset, fileName) {
  if (!window.confirm(`Remove ${fileName} from ${dataset.name}?`)) {
    return;
  }
  try {
    await removeFileFromBrowserDataset(dataset, fileName);
    browserDatasets = await loadBrowserDatasets();
    renderDatasets();
  } catch (error) {
    showDetails({
      ok: false,
      error: {
        stage: 'browser-storage',
        message: error.message || String(error)
      }
    });
    selectTab('details');
  }
}

function initActions() {
  $('#run-button').addEventListener('click', async () => {
    const sql = getSql();
    const dataset = activeDataset();
    if (!dataset) {
      showDetails({ ok: false, error: { message: 'Select a dataset first.' } });
      selectTab('details');
      return;
    }
    if (dataset.source?.kind === 'browser') {
      await runBrowser(sql, dataset);
      return;
    }
    const key = explainKey(sql, dataset);
    if (lastExplainKey !== key) {
      setStatus('explaining');
      const explained = await explainCurrent(sql, dataset, false);
      if (!explained) {
        return;
      }
    }

    setStatus('running');
    const result = await postJson('/api/run', { sql, dataset });
    if (result.ok === false) {
      setStatus('run failed');
      showDetails(result);
      selectTab('details');
      return;
    }
    renderResult(result);
    showDetails({
      format: result.format,
      elapsedMs: result.elapsedMs,
      stderr: result.stderr
    });
    setStatus('finished');
    selectTab('result');
  });

  $('#explain-button').addEventListener('click', async () => {
    await explainCurrent(getSql(), activeDataset(), true);
  });

  window.addEventListener('resize', () => {
    if (editor) {
      editor.setSize('100%', '100%');
      editor.refresh();
    }
    renderCurrentGraph();
  });
}

async function explainCurrent(sql, dataset, selectGraph) {
  if (!dataset) {
    showDetails({ ok: false, error: { message: 'Select a dataset first.' } });
    selectTab('details');
    return false;
  }
  setStatus('explaining');
  const request = {
    sql,
    dataset,
    options: {
      scheduler: 'threaded',
      schedulerWorkers: 18,
      scanTasks: 18,
      shufflePartitions: 4,
      format: 'runtime-bundle',
      embedWasm: true,
      verboseKernels: true
    }
  };
    const bundle = await postJson('/api/explain', request);
    lastBundle = bundle;
    if (bundle.ok === false) {
      setStatus('explain failed');
      showDetails(bundle);
      selectTab('details');
      return false;
    }
    lastExplainKey = explainKey(sql, dataset);
    renderCurrentGraph();
    clearInspector();
    renderPlans(bundle);
    showDetails(bundleSummary(bundle));
    setStatus('explained');
    if (selectGraph) {
      selectTab('graph');
    }
    return true;
}

async function runBrowser(sql, dataset) {
  try {
    setStatus('explaining');
    const request = {
      sql,
      dataset,
      options: {
        scheduler: 'single',
        scanTasks: 1,
        shufflePartitions: 1,
        format: 'runtime-bundle',
        embedWasm: true
      }
    };
    const bundle = await postJson('/api/explain', request);
    lastBundle = bundle;
    if (bundle.ok === false) {
      setStatus('run failed');
      showDetails(bundle);
      selectTab('details');
      return;
    }
    lastExplainKey = explainKey(sql, dataset);
    renderCurrentGraph();
    renderPlans(bundle);

    const exec = bundle.exec;
    if (!exec || exec.supported !== true) {
      setStatus('run failed');
      showDetails({
        ok: false,
        error: {
          stage: 'browser-exec',
          message: exec?.reason ||
            'This query is not supported for browser execution yet.'
        }
      });
      selectTab('details');
      return;
    }

    const resolvedExec = resolveExecArtifacts(exec, bundle.artifacts || {});

    setStatus('running');
    setRunProgress(0);
    const started = performance.now();
    const result = await runBrowserWorker(resolvedExec, dataset, updateRunProgress);
    const elapsedMs = performance.now() - started;
    renderBrowserResult(result, elapsedMs);
    showDetails({
      mode: 'browser',
      rows: result.rows.length,
      elapsedMs,
      timings: result.timings || [],
      scheduler: result.scheduler || null,
      connections: result.connections || []
    });
    setStatus('finished');
    setRunProgress(null);
    selectTab('result');
  } catch (error) {
    setStatus('run failed');
    setRunProgress(null);
    showDetails({
      ok: false,
      error: { stage: 'browser-exec', message: error.message || String(error) }
    });
    selectTab('details');
  }
}

function resolveExecArtifacts(exec, artifacts) {
  const resolveWasm = item => {
    const artifactId = item.wasm;
    if (!artifactId) {
      return item;
    }
    const data = artifacts?.[artifactId]?.data;
    if (!data) {
      throw new Error(`exec artifact is missing: ${artifactId}`);
    }
    return { ...item, wasm: data };
  };
  if (Array.isArray(exec.nodes)) {
    return {
      ...exec,
      nodes: exec.nodes.map(resolveWasm)
    };
  }
  throw new Error('exec plan is missing graph nodes');
}

function runBrowserWorker(exec, dataset, onProgress) {
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL('./browser_worker.js', import.meta.url), {
      type: 'module'
    });
    worker.onmessage = event => {
      const message = event.data || {};
      if (message.type === 'progress') {
        onProgress(message.progress);
      } else if (message.type === 'result') {
        worker.terminate();
        resolve(message.result);
      } else if (message.type === 'error') {
        worker.terminate();
        const error = new Error(message.error?.message || 'browser worker failed');
        error.stack = message.error?.stack || error.stack;
        reject(error);
      }
    };
    worker.onerror = event => {
      worker.terminate();
      reject(new Error(event.message || 'browser worker failed'));
    };
    worker.postMessage({ type: 'run', exec, dataset });
  });
}

function renderBrowserResult(result, elapsedMs) {
  resultRows = [result.columns, ...result.rows];
  resultSort = null;
  renderResultSummary(
    { rows: result.rows.length, elapsedMs, processingMs: elapsedMs },
    resultRows);
  renderResultTable(resultRows, { elapsedMs });
}

function explainKey(sql, dataset) {
  return JSON.stringify({
    sql,
    dataset: dataset?.id || '',
    schemaVersion: dataset?.tables?.length || 0
  });
}

function showSelection(selection) {
  if (selection.type === 'node') {
    showNodeInspector(selection.node);
  } else {
    const graph = currentGraph();
    const connection = (graph?.connections || [])
      .find(item => item.id === selection.edge.connection);
    showInspectorJson('Connection', {
      type: 'connection',
      edge: selection.edge,
      connection
    });
  }
}

function renderCurrentGraph() {
  if (!lastBundle) {
    return;
  }
  renderGraph($('#graph'), currentGraph(), showSelection);
}

function currentGraph() {
  return lastBundle?.graphs?.[graphMode] || lastBundle?.graph || {};
}

function renderPlans(bundle) {
  $('#logical-plan').textContent = bundle.plans?.logicalText || bundle.plan?.logicalText || '';
}

function renderResult(result) {
  resultRows = parseCsv(result.csv || '');
  resultSort = null;
  renderResultSummary(result, resultRows);
  renderResultTable(resultRows, result);
}

function renderResultSummary(result, rows) {
  const rowCount = Number.isFinite(result.rows)
    ? result.rows
    : Math.max(rows.length - 1, 0);
  const processingMs = Number.isFinite(result.processingMs)
    ? result.processingMs
    : result.elapsedMs;
  $('#result-summary').textContent = [
    `Rows: ${formatNumber(rowCount)}`,
    `Processing: ${formatDuration(processingMs)}`,
    `Wall: ${formatDuration(result.elapsedMs)}`
  ].join(' · ');
}

function renderResultTable(rows, result = {}) {
  const head = $('#result-head');
  const body = $('#result-body');
  head.replaceChildren();
  body.replaceChildren();

  if (!rows.length) {
    head.innerHTML = '<tr><th>Status</th><th>Rows</th><th>Time</th></tr>';
    const row = document.createElement('tr');
    row.innerHTML = `<td>OK</td><td>0</td><td>${escapeHtml(String(result.elapsedMs || 0))} ms</td>`;
    body.appendChild(row);
    return;
  }

  const columns = rows[0];
  const dataRows = sortedRows(rows.slice(1));
  const headRow = document.createElement('tr');
  columns.forEach((column, index) => {
    const cell = document.createElement('th');
    const button = document.createElement('button');
    button.className = 'result-sort';
    button.type = 'button';
    button.textContent = `${column}${sortIndicator(index)}`;
    button.addEventListener('click', () => {
      toggleResultSort(index);
      renderResultTable(resultRows, result);
    });
    cell.appendChild(button);
    headRow.appendChild(cell);
  });
  head.appendChild(headRow);

  for (const values of dataRows) {
    const row = document.createElement('tr');
    for (let i = 0; i < columns.length; ++i) {
      const cell = document.createElement('td');
      cell.textContent = values[i] ?? '';
      row.appendChild(cell);
    }
    body.appendChild(row);
  }
}

function toggleResultSort(column) {
  if (!resultSort || resultSort.column !== column) {
    resultSort = { column, direction: 'asc' };
  } else if (resultSort.direction === 'asc') {
    resultSort = { column, direction: 'desc' };
  } else {
    resultSort = null;
  }
}

function sortIndicator(column) {
  if (!resultSort || resultSort.column !== column) {
    return ' ↕';
  }
  return resultSort.direction === 'asc' ? ' ↑' : ' ↓';
}

function sortedRows(rows) {
  if (!resultSort) {
    return rows;
  }
  const direction = resultSort.direction === 'asc' ? 1 : -1;
  const column = resultSort.column;
  return [...rows].sort((left, right) => {
    return compareCell(left[column] ?? '', right[column] ?? '') * direction;
  });
}

function compareCell(left, right) {
  const leftNumber = Number(left);
  const rightNumber = Number(right);
  if (left.trim() !== '' && right.trim() !== '' &&
      Number.isFinite(leftNumber) && Number.isFinite(rightNumber)) {
    return leftNumber - rightNumber;
  }
  return left.localeCompare(right, undefined, { numeric: true });
}

function parseCsv(text) {
  const rows = [];
  let row = [];
  let value = '';
  let quoted = false;
  for (let i = 0; i < text.length; ++i) {
    const ch = text[i];
    if (quoted) {
      if (ch === '"') {
        if (text[i + 1] === '"') {
          value += '"';
          ++i;
        } else {
          quoted = false;
        }
      } else {
        value += ch;
      }
      continue;
    }

    if (ch === '"') {
      quoted = true;
    } else if (ch === ',') {
      row.push(value);
      value = '';
    } else if (ch === '\n') {
      row.push(value);
      rows.push(row);
      row = [];
      value = '';
    } else if (ch !== '\r') {
      value += ch;
    }
  }
  if (value || row.length) {
    row.push(value);
    rows.push(row);
  }
  return rows;
}

function collectArtifacts(node) {
  const result = {};
  const refs = node.artifacts || {};
  const all = lastBundle?.artifacts || {};
  for (const [name, value] of Object.entries(refs)) {
    const ids = Array.isArray(value) ? value : [value];
    result[name] = ids
      .map(id => {
        const item = all[id] || {};
        return {
          id,
          kind: item.kind || name,
          label: item.label || '',
          stage: item.stage || '',
          text: item.text || '',
          data: item.data || '',
          encoding: item.encoding || '',
          byteLength: item.byteLength || 0
        };
      })
      .filter(item => item.text || item.data)
      .filter(item => !item.stage || !node.taskGroup || item.stage === node.taskGroup);
  }
  return result;
}

function showNodeInspector(node) {
  const artifacts = collectArtifacts(node);
  const lines = [
    `id: ${node.id || ''}`,
    `kind: ${node.kind || ''}`,
    `label: ${node.label || ''}`
  ];
  if (node.tooltip) {
    lines.push(`tooltip: ${node.tooltip}`);
  }
  if (node.taskGroup) {
    lines.push(`task group: ${node.taskGroup}`);
  }
  if (node.taskCount) {
    lines.push(`tasks: ${node.taskCount}`);
  }
  if (node.details) {
    lines.push('', 'DETAILS', JSON.stringify(node.details, null, 2));
  }

  inspectorSummaryText = lines.join('\n');
  inspectorArtifactItems = flattenArtifacts(artifacts);
  if (!inspectorArtifactItems.length) {
    lines.push('', 'ARTIFACTS', 'No AST/IR/LLVM artifacts for this node.');
    inspectorSummaryText = lines.join('\n');
  }

  $('#inspector-title').textContent = `Node: ${node.label || node.kind || node.id}`;
  renderInspectorArtifactSelect();
  renderSelectedInspectorArtifact(0);
}

function flattenArtifacts(artifacts) {
  const items = [];
  for (const [name, title] of [
    ['ast', 'AST'],
    ['ir', 'IR'],
    ['llvm', 'LLVM IR'],
    ['wasm', 'WASM']
  ]) {
    const artifactItems = artifacts[name] || [];
    artifactItems.forEach((item, index) => {
      const suffix = artifactItems.length > 1 ? ` ${index + 1}` : '';
      const label = item.label ? ` · ${item.label}` : '';
      items.push({
        title: `${title}${suffix}${label}`,
        text: item.text || [
          `encoding: ${item.encoding || 'base64'}`,
          `byteLength: ${item.byteLength || 0}`,
          '',
          item.data
        ].join('\n')
      });
    });
  }
  return items;
}

function renderInspectorArtifactSelect() {
  const select = $('#inspector-artifact-select');
  select.replaceChildren();
  select.hidden = !inspectorArtifactItems.length;
  if (select.hidden) {
    return;
  }

  const summary = document.createElement('option');
  summary.value = '0';
  summary.textContent = 'Summary';
  select.appendChild(summary);

  inspectorArtifactItems.forEach((item, index) => {
    const option = document.createElement('option');
    option.value = String(index + 1);
    option.textContent = item.title;
    select.appendChild(option);
  });
  select.value = '0';
}

function renderSelectedInspectorArtifact(index) {
  if (index <= 0) {
    $('#inspector-body').textContent = inspectorSummaryText;
    return;
  }
  const item = inspectorArtifactItems[index - 1];
  $('#inspector-body').textContent = item
    ? `${item.title}\n${item.text}`
    : inspectorSummaryText;
}

function showDetails(value) {
  $('#details').textContent = JSON.stringify(value, null, 2);
}

function showInspectorJson(title, value) {
  showInspectorText(title, JSON.stringify(value, null, 2));
}

function showInspectorText(title, value) {
  clearInspectorArtifacts();
  $('#inspector-title').textContent = title;
  $('#inspector-body').textContent = value;
}

function clearInspector() {
  clearInspectorArtifacts();
  $('#inspector-title').textContent = 'Selection';
  $('#inspector-body').textContent = 'Select a node or connection.';
}

function clearInspectorArtifacts() {
  inspectorSummaryText = '';
  inspectorArtifactItems = [];
  const select = $('#inspector-artifact-select');
  select.replaceChildren();
  select.hidden = true;
}

function bundleSummary(bundle) {
  const graph = bundle.graphs?.physical || bundle.graph || {};
  return {
    format: bundle.format,
    version: bundle.version,
    mode: bundle.mode,
    nodes: (graph.nodes || []).length,
    connections: (graph.connections || []).length,
    edges: (graph.edges || []).length
  };
}

function selectTab(name) {
  document.querySelector(`.tab[data-tab="${name}"]`)?.click();
}

function setStatus(text) {
  $('#status').textContent = text;
  $('#status-spinner').hidden = !['explaining', 'running'].includes(text);
}

function setRunProgress(value) {
  const progress = $('#run-progress');
  if (value === null || value === undefined) {
    progress.hidden = true;
    progress.value = 0;
    return;
  }
  progress.hidden = false;
  progress.value = Math.max(0, Math.min(Number(value) || 0, 1));
}

function updateRunProgress(progress) {
  if (!progress || !progress.totalUnits) {
    return;
  }
  setRunProgress(Math.min(progress.completedUnits / progress.totalUnits, 0.95));
}

function formatNumber(value) {
  return Number(value || 0).toLocaleString('en-US');
}

function formatDuration(ms) {
  const value = Number(ms || 0);
  if (value >= 1000) {
    return `${(value / 1000).toFixed(3)} s`;
  }
  return `${value.toFixed(0)} ms`;
}

function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, ch => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;'
  }[ch]));
}
