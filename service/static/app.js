import { getJson, postJson } from './api.js';
import { renderGraph } from './graph.js';
import { tpchQueries } from './tpch_queries.js';

const $ = selector => document.querySelector(selector);

const QUERIES_KEY = 'qdb.web.queries';
const ACTIVE_QUERY_KEY = 'qdb.web.activeQuery';
const DATASETS_KEY = 'qdb.web.datasets';
const ACTIVE_DATASET_KEY = 'qdb.web.activeDataset';

const tpchDataset = {
  id: 'tpch-browser-mock',
  name: 'TPC-H Mock',
  source: { kind: 'browser', files: [] },
  tables: [
    table('region', [['r_regionkey', 'i32'], ['r_name', 'string'], ['r_comment', 'string']], stats(5, 1)),
    table('nation', [['n_nationkey', 'i32'], ['n_name', 'string'], ['n_regionkey', 'i32'], ['n_comment', 'string']], stats(25, 1)),
    table('supplier', [['s_suppkey', 'i32'], ['s_name', 'string'], ['s_address', 'string'], ['s_nationkey', 'i32'], ['s_phone', 'string'], ['s_acctbal', 'f64'], ['s_comment', 'string']], stats(10000, 2)),
    table('customer', [['c_custkey', 'i32'], ['c_name', 'string'], ['c_address', 'string'], ['c_nationkey', 'i32'], ['c_phone', 'string'], ['c_acctbal', 'f64'], ['c_mktsegment', 'string'], ['c_comment', 'string']], stats(150000, 4)),
    table('part', [['p_partkey', 'i32'], ['p_name', 'string'], ['p_mfgr', 'string'], ['p_brand', 'string'], ['p_type', 'string'], ['p_size', 'i32'], ['p_container', 'string'], ['p_retailprice', 'f64'], ['p_comment', 'string']], stats(200000, 4)),
    table('partsupp', [['ps_partkey', 'i32'], ['ps_suppkey', 'i32'], ['ps_availqty', 'i32'], ['ps_supplycost', 'f64'], ['ps_comment', 'string']], stats(800000, 8)),
    table('orders', [['o_orderkey', 'i32'], ['o_custkey', 'i32'], ['o_orderstatus', 'string'], ['o_totalprice', 'f64'], ['o_orderdate', 'i32'], ['o_orderpriority', 'string'], ['o_clerk', 'string'], ['o_shippriority', 'i32'], ['o_comment', 'string']], stats(1500000, 12)),
    table('lineitem', [['l_orderkey', 'i32'], ['l_partkey', 'i32'], ['l_suppkey', 'i32'], ['l_linenumber', 'i32'], ['l_quantity', 'f64'], ['l_extendedprice', 'f64'], ['l_discount', 'f64'], ['l_tax', 'f64'], ['l_returnflag', 'string'], ['l_linestatus', 'string'], ['l_shipdate', 'i32'], ['l_commitdate', 'i32'], ['l_receiptdate', 'i32'], ['l_shipinstruct', 'string'], ['l_shipmode', 'string'], ['l_comment', 'string']], stats(6001215, 18))
  ]
};

let editor = null;
let activeQueryId = localStorage.getItem(ACTIVE_QUERY_KEY) || '';
let suppressEditorSave = false;
let activeDatasetId = localStorage.getItem(ACTIVE_DATASET_KEY) || tpchDataset.id;
let serverDatasets = [];
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

function stats(rows, rowGroups) {
  return {
    rows,
    rowGroups,
    bytes: rows * 128
  };
}

function table(name, columns, tableStats = null) {
  return {
    name,
    columns: columns.map(([columnName, type]) => ({ name: columnName, type })),
    ...(tableStats ? { stats: tableStats } : {})
  };
}

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
  if (!localStorage.getItem(ACTIVE_DATASET_KEY) && serverDatasets.length) {
    activeDatasetId = serverDatasets[0].id;
    localStorage.setItem(ACTIVE_DATASET_KEY, activeDatasetId);
  }

  const existing = loadDatasets();
  const presetIndex = existing.findIndex(dataset => dataset.id === tpchDataset.id);
  if (presetIndex >= 0) {
    existing[presetIndex] = tpchDataset;
    saveDatasets(existing);
  } else {
    existing.unshift(tpchDataset);
    saveDatasets(existing);
  }
  $('#dataset-add-files').addEventListener('click', () => $('#dataset-files').click());
  $('#dataset-files').addEventListener('change', event => {
    const files = Array.from(event.target.files || []).map(file => ({
      name: file.name,
      size: file.size,
      type: file.type || 'application/octet-stream'
    }));
    const datasets = loadDatasets();
    const dataset = {
      id: crypto.randomUUID(),
      name: files.length ? files[0].name : 'Browser dataset',
      source: { kind: 'browser', files },
      tables: []
    };
    datasets.unshift(dataset);
    activeDatasetId = dataset.id;
    localStorage.setItem(ACTIVE_DATASET_KEY, activeDatasetId);
    saveDatasets(datasets);
    renderDatasets();
  });
  renderDatasets();
}

async function loadServerDatasets() {
  const result = await getJson('/api/datasets');
  if (result.ok === false) {
    return [];
  }
  return result.datasets || [];
}

function loadDatasets() {
  try {
    return JSON.parse(localStorage.getItem(DATASETS_KEY) || '[]');
  } catch {
    return [];
  }
}

function saveDatasets(datasets) {
  localStorage.setItem(DATASETS_KEY, JSON.stringify(datasets));
}

function activeDataset() {
  return allDatasets().find(dataset => dataset.id === activeDatasetId) || tpchDataset;
}

function allDatasets() {
  return [...serverDatasets, ...loadDatasets()];
}

function renderDatasets() {
  const root = $('#datasets-list');
  root.replaceChildren();
  for (const dataset of allDatasets()) {
    const button = document.createElement('button');
    button.className = `list-item${dataset.id === activeDatasetId ? ' active' : ''}`;
    button.type = 'button';
    button.textContent = dataset.source?.kind === 'server'
      ? `${dataset.name} · server`
      : dataset.name;
    button.addEventListener('click', () => {
      activeDatasetId = dataset.id;
      localStorage.setItem(ACTIVE_DATASET_KEY, activeDatasetId);
      renderDatasets();
    });
    root.appendChild(button);
  }
  renderSchema(activeDataset());
}

function renderSchema(dataset) {
  const root = $('#schema-tree');
  root.replaceChildren();
  for (const item of dataset.tables || []) {
    const tableRoot = document.createElement('div');
    tableRoot.className = 'schema-table';
    const title = document.createElement('div');
    title.className = 'schema-table-name';
    title.textContent = item.stats
      ? `${item.name} · ${formatNumber(item.stats.rows || 0)} rows · ${item.stats.rowGroups || 1} rg`
      : item.name;
    tableRoot.appendChild(title);
    for (const column of item.columns || []) {
      const row = document.createElement('div');
      row.className = 'schema-column';
      row.innerHTML = `<span>${escapeHtml(column.name)}</span><span>${escapeHtml(column.type)}</span>`;
      tableRoot.appendChild(row);
    }
    root.appendChild(tableRoot);
  }
}

function initActions() {
  $('#run-button').addEventListener('click', async () => {
    const sql = getSql();
    const dataset = activeDataset();
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
          text: item.text || ''
        };
      })
      .filter(item => item.text)
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
    ['llvm', 'LLVM IR']
  ]) {
    const artifactItems = artifacts[name] || [];
    artifactItems.forEach((item, index) => {
      const suffix = artifactItems.length > 1 ? ` ${index + 1}` : '';
      const label = item.label ? ` · ${item.label}` : '';
      items.push({
        title: `${title}${suffix}${label}`,
        text: item.text
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
