import { getJson, postJson } from './api.js';
import { renderGraph } from './graph.js';
import { tpchQueries } from './tpch_queries.js';
import {
  addFilesToBrowserDataset,
  addStoredFilesToBrowserDataset,
  createBrowserDataset,
  deleteBrowserDataset,
  listBrowserDatasets,
  removeFileFromBrowserDataset,
  renameBrowserDataset,
  writeOpfsFileStream,
  writeOpfsStoredFile,
  getWorkspaceState,
  putWorkspaceState,
  deleteWorkspaceState
} from './browser_storage.js';
import { readParquetTable } from './browser_parquet.js';

const $ = selector => document.querySelector(selector);

const QUERIES_KEY = 'qdb.web.queries';
const ACTIVE_QUERY_KEY = 'qdb.web.activeQuery';
const ACTIVE_DATASET_KEY = 'qdb.web.activeDataset';
const LAST_RUN_KEY = 'qdb.web.lastRun';
const LOCAL_DATA_DOWNLOAD_CONCURRENCY = 4;
const MAX_PERSISTED_RESULT_ROWS = 1000;

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
let resultMeta = null;
let inspectorSummaryText = '';
let inspectorArtifactItems = [];
let toastTimer = 0;

// Execution queue: at most one job runs at a time. `runningJob` is the current
// one; `runQueue` holds pending jobs in order. A job is bound to the query +
// dataset it was launched for so its result lands there regardless of what the
// user is viewing.
let runningJob = null;
const runQueue = [];
let jobSeq = 0;

window.addEventListener('DOMContentLoaded', () => {
  window.lucide?.createIcons();
  initEditor();
  initDrawers();
  initTabs();
  initGraphControls();
  initDiagnosticCopy();
  initQueries();
  initDatasets();
  initActions();
  initSourceDownload();
});

async function initSourceDownload() {
  const link = $('#source-download');
  if (!link) {
    return;
  }
  const info = await getJson('/api/version');
  if (info && info.sourceAvailable) {
    link.hidden = false;
    window.lucide?.createIcons();
  }
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
      updateGraphModeButtons();
      renderCurrentGraph();
      clearInspector();
      saveWorkspaceState();
    });
  }
  $('#inspector-clear').addEventListener('click', clearInspector);
  $('#inspector-artifact-select').addEventListener('change', event => {
    renderSelectedInspectorArtifact(Number(event.target.value || 0));
  });
}

function initDiagnosticCopy() {
  bindCopyButton('#inspector-copy', () => $('#inspector-body').textContent || '');
  bindCopyButton('#logical-copy', () => $('#logical-plan').textContent || '');
  bindCopyButton('#details-copy', () => $('#details').textContent || '');
}

function bindCopyButton(buttonSelector, readText) {
  const button = $(buttonSelector);
  if (!button) {
    return;
  }
  const defaultTitle = button.title;
  const defaultLabel = button.getAttribute('aria-label') || defaultTitle;
  button.addEventListener('click', async () => {
    const text = readText();
    if (!text) {
      return;
    }
    try {
      await copyText(text);
      button.title = 'Copied';
      button.setAttribute('aria-label', 'Copied');
    } catch (error) {
      button.title = 'Copy failed';
      button.setAttribute('aria-label', 'Copy failed');
      console.warn('copy failed', error);
    }
    setTimeout(() => {
      button.title = defaultTitle;
      button.setAttribute('aria-label', defaultLabel);
    }, 1200);
  });
}

async function copyText(text) {
  if (navigator.clipboard?.writeText) {
    await navigator.clipboard.writeText(text);
    return;
  }
  const textarea = document.createElement('textarea');
  textarea.value = text;
  textarea.setAttribute('readonly', '');
  textarea.style.position = 'fixed';
  textarea.style.left = '-9999px';
  document.body.appendChild(textarea);
  textarea.select();
  document.execCommand('copy');
  textarea.remove();
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

  $('#query-new').addEventListener('click', async () => {
    saveActiveQuerySql();
    saveWorkspaceState();
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
    await restoreWorkspaceState();
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
    button.className = `list-item query-item${query.id === activeQueryId ? ' active' : ''}`;
    button.type = 'button';
    const label = document.createElement('span');
    label.className = 'query-name';
    label.textContent = query.name;
    button.appendChild(label);

    const lastRun = loadLastRun(query.id);
    if (lastRun) {
      const meta = document.createElement('span');
      meta.className = 'query-meta';
      meta.textContent = formatDuration(lastRun.processingMs ?? lastRun.elapsedMs);
      meta.title = `Last run ${formatRelativeTime(lastRun.at)}`;
      button.appendChild(meta);
    }

    const state = queryRunState(query.id);
    if (state) {
      const indicator = document.createElement('span');
      indicator.className = state === 'running' ? 'query-spinner' : 'query-queued';
      indicator.title = state === 'running' ? 'Running' : 'Queued';
      button.appendChild(indicator);

      const stop = document.createElement('span');
      stop.className = 'query-stop';
      stop.setAttribute('role', 'button');
      stop.setAttribute('aria-label', state === 'running' ? 'Stop' : 'Remove from queue');
      stop.title = state === 'running' ? 'Stop' : 'Remove from queue';
      stop.textContent = '✕';
      stop.addEventListener('click', event => {
        event.stopPropagation();
        stopQuery(query.id);
      });
      button.appendChild(stop);
    }

    button.addEventListener('click', async () => {
      if (query.id === activeQueryId) {
        return;
      }
      saveActiveQuerySql();
      saveWorkspaceState();
      activeQueryId = query.id;
      localStorage.setItem(ACTIVE_QUERY_KEY, activeQueryId);
      const freshQuery = loadQueries().find(item => item.id === activeQueryId);
      setSql(freshQuery?.sql || '');
      // The status label reflects live activity, not the selected query, so a
      // run still in flight for the previous query must not read as this one's.
      setStatus('idle');
      renderQueries();
      await restoreWorkspaceState();
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
  await restoreWorkspaceState();
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
    switchActiveDataset(dataset.id);
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

function switchActiveDataset(datasetId) {
  if (datasetId === activeDatasetId) {
    return;
  }
  saveWorkspaceState();
  activeDatasetId = datasetId || '';
  if (activeDatasetId) {
    localStorage.setItem(ACTIVE_DATASET_KEY, activeDatasetId);
  } else {
    localStorage.removeItem(ACTIVE_DATASET_KEY);
  }
  restoreWorkspaceState();
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

async function downloadLocalDataset(dataset) {
  const source = dataset.source || {};
  const files = source.files || [];
  if (source.kind !== 'local' || !files.length) {
    showDetails({ ok: false, error: { message: 'No downloadable parquet files.' } });
    selectTab('details');
    return;
  }

  const existing = downloadedBrowserDatasetFor(dataset);
  const target = existing || {
    id: crypto.randomUUID(),
    name: dataset.name,
    source: {
      kind: 'browser',
      storage: 'opfs',
      files: [],
      downloadedFrom: {
        id: dataset.id,
        name: dataset.name,
        path: source.path || ''
      }
    },
    tables: []
  };

  setStatus('downloading');
  setRunProgress(0);
  showToast(`Download started: ${dataset.name}`);
  try {
    const totalBytes = Math.max(
      files.reduce((sum, item) => sum + Math.max(Number(item.size || 0), 0), 0),
      1);
    const progressByFile = new Array(files.length).fill(0);
    const updateFileProgress = (index, bytes) => {
      progressByFile[index] = Math.max(progressByFile[index], bytes);
      const completedBytes = progressByFile.reduce((sum, value) => sum + value, 0);
      setRunProgress(completedBytes / totalBytes);
    };
    const storedFiles = await downloadLocalDataFiles(
      target.id,
      files,
      LOCAL_DATA_DOWNLOAD_CONCURRENCY,
      updateFileProgress);

    const tableByFile = new Map((dataset.tables || []).map(table => [
      table.sourceFile || `${table.name}.parquet`,
      table
    ]));
    const tables = storedFiles.map(file => {
      const table = tableByFile.get(file.name);
      if (!table) {
        throw new Error(`missing table metadata for ${file.name}`);
      }
      return {
        ...table,
        sourceFile: file.name
      };
    });
    await addStoredFilesToBrowserDataset(target, storedFiles, tables);
    browserDatasets = await loadBrowserDatasets();
    const updated = browserDatasets.find(item =>
      item.source?.downloadedFrom?.id === dataset.id) ||
      browserDatasets.find(item => item.id === target.id);
    switchActiveDataset(updated?.id || target.id);
    renderDatasets();
    setStatus('dataset ready');
    setRunProgress(null);
  } catch (error) {
    setStatus('download failed');
    setRunProgress(null);
    showDetails({
      ok: false,
      error: {
        stage: 'local-data',
        message: error.message || String(error)
      }
    });
    selectTab('details');
  }
}

async function downloadLocalDataFiles(datasetId, files, concurrency, onProgress) {
  const controller = new AbortController();
  try {
    return await parallelMapLimit(files, concurrency, (item, index) =>
      downloadLocalDataFile(datasetId, item, {
        signal: controller.signal,
        onProgress: bytes => onProgress(index, bytes)
      }));
  } catch (error) {
    controller.abort();
    throw error;
  }
}

async function downloadLocalDataFile(datasetId, item, options) {
  const response = await fetch(item.url, { signal: options.signal });
  if (!response.ok) {
    throw new Error(`failed to download ${item.name}: ${response.statusText}`);
  }

  if (!response.body?.getReader) {
    const blob = await response.blob();
    const bytes = blob.size || Number(item.size || 0);
    options.onProgress?.(bytes);
    return writeOpfsStoredFile(datasetId, new File(
      [blob],
      item.name,
      { type: blob.type || response.headers.get('Content-Type') || 'application/octet-stream' }));
  }

  return writeOpfsFileStream(datasetId, item.name, response.body, {
    type: response.headers.get('Content-Type') || 'application/octet-stream',
    onProgress: bytes => options.onProgress?.(bytes)
  });
}

async function parallelMapLimit(items, limit, fn) {
  const results = new Array(items.length);
  let nextIndex = 0;
  const workerCount = Math.max(1, Math.min(Number(limit) || 1, items.length));
  const workers = Array.from({ length: workerCount }, async () => {
    for (;;) {
      const index = nextIndex++;
      if (index >= items.length) {
        break;
      }
      results[index] = await fn(items[index], index);
    }
  });
  await Promise.all(workers);
  return results;
}

function downloadedBrowserDatasetFor(dataset) {
  return browserDatasets.find(item => item.source?.downloadedFrom?.id === dataset.id) || null;
}

async function deleteDownloadedLocalDataset(dataset) {
  const downloaded = downloadedBrowserDatasetFor(dataset);
  if (!downloaded) {
    return;
  }
  if (!window.confirm(`Delete OPFS copy of ${dataset.name}?`)) {
    return;
  }
  try {
    await deleteBrowserDataset(downloaded.id);
    browserDatasets = await loadBrowserDatasets();
    switchActiveDataset(dataset.id);
    renderDatasets();
    setStatus('dataset deleted');
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
    let actionCount = 0;
    const button = document.createElement('button');
    button.className = 'dataset-select';
    button.type = 'button';
    button.textContent = `${dataset.name} · ${datasetKindLabel(dataset)}`;
    button.addEventListener('click', () => {
      if (dataset.id === activeDatasetId) {
        return;
      }
      switchActiveDataset(dataset.id);
      renderDatasets();
    });
    row.appendChild(button);
    if (dataset.source?.kind === 'local') {
      const downloaded = downloadedBrowserDatasetFor(dataset);
      const download = document.createElement('button');
      download.className = 'icon-button dataset-action';
      download.type = 'button';
      download.title = 'Download to OPFS';
      download.setAttribute('aria-label', 'Download to OPFS');
      download.innerHTML = '<i data-lucide="download"></i>';
      download.addEventListener('click', async event => {
        event.stopPropagation();
        switchActiveDataset(dataset.id);
        renderDatasets();
        await downloadLocalDataset(dataset);
      });
      row.appendChild(download);
      actionCount += 1;

      if (downloaded) {
        const removeDownloaded = document.createElement('button');
        removeDownloaded.className = 'icon-button dataset-action';
        removeDownloaded.type = 'button';
        removeDownloaded.title = 'Delete OPFS copy';
        removeDownloaded.setAttribute('aria-label', 'Delete OPFS copy');
        removeDownloaded.innerHTML = '<i data-lucide="trash-2"></i>';
        removeDownloaded.addEventListener('click', async event => {
          event.stopPropagation();
          await deleteDownloadedLocalDataset(dataset);
        });
        row.appendChild(removeDownloaded);
        actionCount += 1;
      }
    }
    if (dataset.source?.kind === 'browser') {
      const upload = document.createElement('button');
      upload.className = 'icon-button dataset-action';
      upload.type = 'button';
      upload.title = 'Add parquet files';
      upload.setAttribute('aria-label', 'Add parquet files');
      upload.innerHTML = '<i data-lucide="upload"></i>';
      upload.addEventListener('click', event => {
        event.stopPropagation();
        switchActiveDataset(dataset.id);
        renderDatasets();
        $('#dataset-files').click();
      });
      row.appendChild(upload);
      actionCount += 1;

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
      actionCount += 1;

      const remove = document.createElement('button');
      remove.className = 'icon-button dataset-action';
      remove.type = 'button';
      remove.title = 'Delete dataset';
      remove.setAttribute('aria-label', 'Delete dataset');
      remove.innerHTML = '<i data-lucide="trash-2"></i>';
      remove.addEventListener('click', async event => {
        event.stopPropagation();
        const removedActive = dataset.id === activeDatasetId;
        await deleteBrowserDataset(dataset.id);
        browserDatasets = await loadBrowserDatasets();
        const previousDatasetId = activeDatasetId;
        ensureActiveDataset();
        if (removedActive || activeDatasetId !== previousDatasetId) {
          restoreWorkspaceState();
        }
        renderDatasets();
      });
      row.appendChild(remove);
      actionCount += 1;
    }
    if (actionCount > 0) {
      row.classList.add('has-actions');
      row.style.setProperty('--dataset-actions', String(actionCount));
    }
    root.appendChild(row);
  }
  window.lucide?.createIcons();
  renderSchema(activeDataset());
}

function datasetKindLabel(dataset) {
  if (dataset.source?.kind === 'server') {
    return 'server';
  }
  if (dataset.source?.kind === 'local') {
    return downloadedBrowserDatasetFor(dataset) ? 'downloaded' : 'download';
  }
  if (dataset.source?.downloadedFrom) {
    return 'browser copy';
  }
  return 'browser';
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
  $('#run-button').addEventListener('click', () => {
    enqueueRun(getSql(), activeDataset());
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

// Returns 'running' | 'queued' | null for a query id, used to draw the list
// indicator. Keyed by query id only (a query appears once in the list).
function queryRunState(queryId) {
  if (runningJob && runningJob.queryId === queryId) {
    return 'running';
  }
  if (runQueue.some(job => job.queryId === queryId)) {
    return 'queued';
  }
  return null;
}

// Enqueues a run of the given query+dataset. Rules:
//  - running the SAME query that is already running cancels it and re-queues
//    (restart);
//  - running a query that is already queued is ignored (no duplicates);
//  - otherwise it joins the queue; the in-flight run keeps going.
function enqueueRun(sql, dataset) {
  if (!dataset) {
    showDetails({ ok: false, error: { message: 'Select a dataset first.' } });
    selectTab('details');
    return;
  }
  if (dataset.source?.kind === 'local') {
    showDetails({
      ok: false,
      error: {
        message: 'Download this dataset to OPFS before running it in the browser.'
      }
    });
    selectTab('details');
    return;
  }

  const queryId = activeQueryId;
  const datasetId = activeDatasetId;
  const sameRunning = runningJob &&
    runningJob.queryId === queryId &&
    runningJob.datasetId === datasetId;
  const alreadyQueued = runQueue.some(job =>
    job.queryId === queryId && job.datasetId === datasetId);

  if (sameRunning) {
    cancelJob(runningJob);
    if (alreadyQueued) {
      return;
    }
  } else if (alreadyQueued) {
    return;
  }

  runQueue.push({
    id: ++jobSeq,
    runId: (crypto.randomUUID?.() ||
      `run-${Date.now()}-${Math.random().toString(16).slice(2)}`),
    queryId,
    datasetId,
    sql,
    dataset,
    kind: dataset.source?.kind === 'browser' ? 'browser' : 'server',
    cancelled: false,
    cancel: null
  });
  renderQueries();
  pumpQueue();
}

function cancelJob(job) {
  job.cancelled = true;
  if (typeof job.cancel === 'function') {
    try {
      job.cancel();
    } catch {
      // ignore
    }
  }
}

// Stop button on a query list item: drops any queued entries for the query and
// cancels it if it is the one currently running. Unlike re-running, this does
// not re-enqueue.
function stopQuery(queryId) {
  for (let i = runQueue.length - 1; i >= 0; i--) {
    if (runQueue[i].queryId === queryId) {
      runQueue.splice(i, 1);
    }
  }
  if (runningJob && runningJob.queryId === queryId) {
    if (isActiveWorkspace(runningJob.queryId, runningJob.datasetId)) {
      setStatus('idle');
    }
    cancelJob(runningJob);
  }
  renderQueries();
}

// Pulls the next job and runs it; enforces a single concurrent runner. When a
// job finishes it clears `runningJob` and pumps the next one.
function pumpQueue() {
  if (runningJob) {
    return;
  }
  const job = runQueue.shift();
  if (!job) {
    renderQueries();
    return;
  }
  runningJob = job;
  renderQueries();
  const runner = job.kind === 'browser' ? executeBrowserJob : executeServerJob;
  Promise.resolve()
    .then(() => runner(job))
    .catch(error => console.error('run job failed', error))
    .finally(() => {
      runningJob = null;
      renderQueries();
      pumpQueue();
    });
}

async function executeServerJob(job) {
  const { sql, dataset, queryId, datasetId, runId } = job;
  const controller = new AbortController();
  const query = `?runId=${encodeURIComponent(runId)}`;
  job.cancel = () => {
    controller.abort();
    // Also kill the child process server-side so qdb stops crunching, not just
    // the client fetch.
    postJson(`/api/cancel${query}`, {}).catch(() => {});
  };
  const viewing = () => isActiveWorkspace(queryId, datasetId);

  try {
    const key = explainKey(sql, dataset);
    if (lastExplainKey !== key) {
      const explained = await explainCurrent(
        sql, dataset, false, controller.signal, queryId, datasetId, runId);
      if (job.cancelled || !explained) {
        return;
      }
    }
    if (viewing()) {
      setStatus('running');
    }
    const result = await postJson(
      `/api/run${query}`, { sql, dataset }, controller.signal);
    if (job.cancelled) {
      return;
    }
    if (result.ok === false) {
      applyOutcome(queryId, datasetId,
        () => {
          setStatus('run failed');
          showDetails(result);
          selectTab('details');
        },
        () => persistErrorToSlot(queryId, datasetId, result));
      return;
    }
    recordLastRun(queryId, datasetId, {
      processingMs: result.processingMs,
      elapsedMs: result.elapsedMs,
      rows: result.rows
    });
    applyOutcome(queryId, datasetId,
      () => {
        renderResult(result);
        showDetails({
          format: result.format,
          elapsedMs: result.elapsedMs,
          stderr: result.stderr
        });
        setStatus('finished');
        selectTab('result');
      },
      () => persistRunResultToSlot(queryId, datasetId, result));
  } catch (error) {
    if (job.cancelled || error.name === 'AbortError') {
      return;
    }
    const payload = {
      ok: false,
      error: { stage: 'run', message: error.message || String(error) }
    };
    applyOutcome(queryId, datasetId,
      () => {
        setStatus('run failed');
        showDetails(payload);
        selectTab('details');
      },
      () => persistErrorToSlot(queryId, datasetId, payload));
  }
}

async function explainCurrent(
    sql, dataset, selectGraph, signal,
    originQueryId = activeQueryId, originDatasetId = activeDatasetId, runId = '') {
  if (!dataset) {
    showDetails({ ok: false, error: { message: 'Select a dataset first.' } });
    selectTab('details');
    return false;
  }
  if (isActiveWorkspace(originQueryId, originDatasetId)) {
    setStatus('explaining');
  }
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
    const explainPath = runId
      ? `/api/explain?runId=${encodeURIComponent(runId)}`
      : '/api/explain';
    let bundle;
    try {
      bundle = await postJson(explainPath, request, signal);
    } catch (error) {
      if (error.name === 'AbortError') {
        return false;
      }
      bundle = {
        ok: false,
        error: { stage: 'explain', message: error.message || String(error) }
      };
    }
    if (bundle.ok === false) {
      applyOutcome(originQueryId, originDatasetId,
        () => {
          lastBundle = bundle;
          setStatus('explain failed');
          showDetails(bundle);
          selectTab('details');
        },
        () => persistErrorToSlot(originQueryId, originDatasetId, bundle));
      return false;
    }
    applyOutcome(originQueryId, originDatasetId,
      () => {
        lastBundle = bundle;
        lastExplainKey = explainKey(sql, dataset);
        renderCurrentGraph();
        clearInspector();
        renderPlans(bundle);
        showDetails(bundleSummary(bundle));
        setStatus('explained');
        if (selectGraph) {
          selectTab('graph');
        }
      },
      () => persistBundleToSlot(originQueryId, originDatasetId, bundle));
    return true;
}

async function executeBrowserJob(job) {
  const { sql, dataset, queryId, datasetId, runId } = job;
  const controller = new AbortController();
  let activeWorker = null;
  let rejectWorker = null;
  job.cancel = () => {
    controller.abort();
    // Kill the server-side explain child (qdb_plan_export) if it is still going.
    postJson(`/api/cancel?runId=${encodeURIComponent(runId)}`, {}).catch(() => {});
    if (activeWorker) {
      activeWorker.terminate();
    }
    if (rejectWorker) {
      rejectWorker(new Error('cancelled'));
    }
  };
  const viewing = () => isActiveWorkspace(queryId, datasetId);

  try {
    if (viewing()) {
      setStatus('explaining');
    }
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
    const bundle = await postJson(
      `/api/explain?runId=${encodeURIComponent(runId)}`, request, controller.signal);
    if (job.cancelled) {
      return;
    }
    if (bundle.ok === false) {
      applyOutcome(queryId, datasetId,
        () => {
          lastBundle = bundle;
          setStatus('run failed');
          showDetails(bundle);
          selectTab('details');
        },
        () => persistErrorToSlot(queryId, datasetId, bundle));
      return;
    }
    applyOutcome(queryId, datasetId,
      () => {
        lastBundle = bundle;
        lastExplainKey = explainKey(sql, dataset);
        renderCurrentGraph();
        renderPlans(bundle);
      },
      () => persistBundleToSlot(queryId, datasetId, bundle));

    const exec = bundle.exec;
    if (!exec || exec.supported !== true) {
      const payload = {
        ok: false,
        error: {
          stage: 'browser-exec',
          message: exec?.reason ||
            'This query is not supported for browser execution yet.'
        }
      };
      applyOutcome(queryId, datasetId,
        () => {
          setStatus('run failed');
          showDetails(payload);
          selectTab('details');
        },
        () => persistErrorToSlot(queryId, datasetId, payload));
      return;
    }

    const resolvedExec = resolveExecArtifacts(exec, bundle.artifacts || {});

    if (viewing()) {
      setStatus('running');
      setRunProgress(0);
    }
    const started = performance.now();
    const result = await runBrowserWorker(
      resolvedExec,
      dataset,
      progress => {
        if (viewing()) {
          updateRunProgress(progress);
        }
      },
      (worker, reject) => { activeWorker = worker; rejectWorker = reject; });
    if (job.cancelled) {
      return;
    }
    const elapsedMs = performance.now() - started;
    const details = {
      mode: 'browser',
      planner: bundle.planner || null,
      rows: result.rows.length,
      elapsedMs,
      timings: result.timings || [],
      scheduler: result.scheduler || null,
      memory: result.memory || null,
      connections: result.connections || []
    };
    recordLastRun(queryId, datasetId, {
      processingMs: elapsedMs,
      elapsedMs,
      rows: result.rows.length
    });
    applyOutcome(queryId, datasetId,
      () => {
        renderBrowserResult(result, elapsedMs);
        showDetails(details);
        setStatus('finished');
        setRunProgress(null);
        selectTab('result');
      },
      () => persistBrowserResultToSlot(
        queryId, datasetId, result, elapsedMs, details));
  } catch (error) {
    if (job.cancelled || error.name === 'AbortError') {
      return;
    }
    const payload = {
      ok: false,
      error: { stage: 'browser-exec', message: error.message || String(error) }
    };
    applyOutcome(queryId, datasetId,
      () => {
        setStatus('run failed');
        setRunProgress(null);
        showDetails(payload);
        selectTab('details');
      },
      () => persistErrorToSlot(queryId, datasetId, payload));
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

function runBrowserWorker(exec, dataset, onProgress, onWorker) {
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL('./browser_worker.js', import.meta.url), {
      type: 'module'
    });
    onWorker?.(worker, reject);
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
  resultMeta = { rows: result.rows.length, elapsedMs, processingMs: elapsedMs };
  renderResultSummary(
    resultMeta,
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

function updateGraphModeButtons() {
  for (const button of document.querySelectorAll('.graph-mode')) {
    button.classList.toggle('active', (button.dataset.mode || 'logical') === graphMode);
  }
}

function renderPlans(bundle) {
  $('#logical-plan').textContent = bundle.plans?.logicalText || bundle.plan?.logicalText || '';
}

function renderResult(result) {
  resultRows = parseCsv(result.csv || '');
  resultSort = null;
  resultMeta = {
    rows: result.rows,
    elapsedMs: result.elapsedMs,
    processingMs: result.processingMs
  };
  renderResultSummary(resultMeta, resultRows);
  renderResultTable(resultRows, resultMeta);
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
      saveWorkspaceState();
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
  saveWorkspaceState();
}

async function restoreWorkspaceState(queryId = activeQueryId, datasetId = activeDatasetId) {
  const state = await loadWorkspaceState(queryId, datasetId);
  if (!state) {
    clearWorkspaceStateView();
    return;
  }

  graphMode = state.graphMode || 'logical';
  updateGraphModeButtons();

  if (state.bundle) {
    lastBundle = state.bundle;
    renderCurrentGraph();
    renderPlans(lastBundle);
  } else {
    lastBundle = null;
    renderGraph($('#graph'), {}, showSelection);
    $('#logical-plan').textContent = '';
  }

  if (typeof state.detailsText === 'string') {
    $('#details').textContent = state.detailsText;
  } else {
    $('#details').textContent = '';
  }

  const storedRows = capResultRows(state.result?.rows || []);
  if (storedRows.length) {
    resultRows = storedRows;
    resultSort = state.result?.sort || null;
    resultMeta = state.result?.meta || {
      rows: Math.max(storedRows.length - 1, 0),
      elapsedMs: 0,
      processingMs: 0
    };
    renderResultSummary(resultMeta, resultRows);
    renderResultTable(resultRows, resultMeta);
  } else {
    clearResultView();
  }
  clearInspector();
}

async function loadWorkspaceState(queryId = activeQueryId, datasetId = activeDatasetId) {
  if (!queryId || !datasetId) {
    return null;
  }
  // See any writes queued for this slot before reading it.
  await workspaceWriteChain;
  try {
    return (await getWorkspaceState(workspaceStateKey(queryId, datasetId))) || null;
  } catch (error) {
    console.warn('workspace load failed', error);
    return null;
  }
}

function saveWorkspaceState(queryId = activeQueryId, datasetId = activeDatasetId) {
  if (!queryId || !datasetId) {
    return;
  }
  const state = {
    version: 1,
    graphMode,
    bundle: compactBundleForStorage(lastBundle),
    detailsText: $('#details')?.textContent || '',
    result: {
      rows: capResultRows(resultRows),
      sort: resultSort,
      meta: resultMeta
    }
  };
  const key = workspaceStateKey(queryId, datasetId);
  return queueWorkspaceWrite(() => putWorkspaceState(key, state));
}

function workspaceStateKey(queryId, datasetId) {
  return `${queryId}:${datasetId}`;
}

// True when the given query+dataset is still the one on screen. Async run/explain
// completions use this to decide between updating the live view and persisting
// their result to the originating query's slot instead.
function isActiveWorkspace(queryId, datasetId) {
  return queryId === activeQueryId && datasetId === activeDatasetId;
}

// Routes an async run/explain outcome: if its query+dataset is still on screen,
// update the live view; otherwise persist it to that query's slot in the
// background so switching back shows it.
function applyOutcome(queryId, datasetId, live, persist) {
  if (isActiveWorkspace(queryId, datasetId)) {
    live();
  } else {
    persist();
  }
}

// All workspace-state writes go through this serialized chain so that
// read-modify-write merges (writeWorkspaceSlot) never race each other or a
// full save. Reads (loadWorkspaceState) await the chain first to see pending
// writes.
let workspaceWriteChain = Promise.resolve();
function queueWorkspaceWrite(task) {
  workspaceWriteChain = workspaceWriteChain.then(task).catch(error => {
    console.warn('workspace persist failed', error);
  });
  return workspaceWriteChain;
}

function writeWorkspaceSlot(queryId, datasetId, patch) {
  if (!queryId || !datasetId) {
    return;
  }
  const key = workspaceStateKey(queryId, datasetId);
  return queueWorkspaceWrite(async () => {
    const state = (await getWorkspaceState(key)) ||
      { version: 1, graphMode: 'logical' };
    Object.assign(state, patch);
    await putWorkspaceState(key, state);
  });
}

function persistBundleToSlot(queryId, datasetId, bundle) {
  writeWorkspaceSlot(queryId, datasetId, {
    bundle: compactBundleForStorage(bundle),
    detailsText: JSON.stringify(bundleSummary(bundle), null, 2)
  });
}

function persistRunResultToSlot(queryId, datasetId, result) {
  writeWorkspaceSlot(queryId, datasetId, {
    result: {
      rows: capResultRows(parseCsv(result.csv || '')),
      sort: null,
      meta: {
        rows: result.rows,
        elapsedMs: result.elapsedMs,
        processingMs: result.processingMs
      }
    },
    detailsText: JSON.stringify({
      format: result.format,
      elapsedMs: result.elapsedMs,
      stderr: result.stderr
    }, null, 2)
  });
}

function persistBrowserResultToSlot(queryId, datasetId, result, elapsedMs, details) {
  writeWorkspaceSlot(queryId, datasetId, {
    result: {
      rows: capResultRows([result.columns, ...result.rows]),
      sort: null,
      meta: { rows: result.rows.length, elapsedMs, processingMs: elapsedMs }
    },
    detailsText: JSON.stringify(details, null, 2)
  });
}

function persistErrorToSlot(queryId, datasetId, payload) {
  writeWorkspaceSlot(queryId, datasetId, {
    detailsText: JSON.stringify(payload, null, 2)
  });
}

function clearWorkspaceStateView() {
  lastBundle = null;
  lastExplainKey = null;
  graphMode = 'logical';
  updateGraphModeButtons();
  renderGraph($('#graph'), {}, showSelection);
  $('#logical-plan').textContent = '';
  $('#details').textContent = '';
  clearResultView();
  clearInspector();
}

function clearResultView() {
  resultRows = [];
  resultSort = null;
  resultMeta = null;
  $('#result-summary').textContent = 'not run';
  $('#result-head').innerHTML = '<tr><th>Status</th><th>Rows</th><th>Time</th></tr>';
  $('#result-body').innerHTML = '<tr><td>not run</td><td>0</td><td>0 ms</td></tr>';
}

function compactBundleForStorage(bundle) {
  if (!bundle) {
    return null;
  }
  // Note: `exec` and `artifacts` are intentionally NOT stored. They are the
  // largest part of a bundle (per-node plans, wasm) and are never read back on
  // restore — only graph/plans are used to redraw. Keeping them here is what
  // blew past the localStorage quota for heavy queries (e.g. TPC-H q13).
  return {
    ok: bundle.ok,
    format: bundle.format,
    version: bundle.version,
    mode: bundle.mode,
    planner: bundle.planner || null,
    graph: bundle.graph || null,
    graphs: bundle.graphs || null,
    plan: bundle.plan || null,
    plans: bundle.plans || null
  };
}

function capResultRows(rows) {
  if (!Array.isArray(rows) || !rows.length) {
    return [];
  }
  return [
    rows[0],
    ...rows.slice(1, MAX_PERSISTED_RESULT_ROWS + 1)
  ];
}

function showToast(message, timeoutMs = 2600) {
  const toast = $('#toast');
  if (!toast) {
    return;
  }
  window.clearTimeout(toastTimer);
  toast.textContent = message;
  toast.hidden = false;
  toastTimer = window.setTimeout(() => {
    toast.hidden = true;
  }, timeoutMs);
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
    planner: bundle.planner || null,
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

function lastRunKey(queryId) {
  return `${LAST_RUN_KEY}:${queryId}`;
}

// Records when a query last finished running (and how long it took), so the
// query list can show it. Keyed by query id.
function recordLastRun(queryId, datasetId, meta) {
  if (!queryId) {
    return;
  }
  try {
    localStorage.setItem(lastRunKey(queryId), JSON.stringify({
      at: Date.now(),
      datasetId,
      processingMs: meta.processingMs,
      elapsedMs: meta.elapsedMs,
      rows: meta.rows
    }));
  } catch {
    // ignore quota errors
  }
}

function loadLastRun(queryId) {
  try {
    return JSON.parse(localStorage.getItem(lastRunKey(queryId)) || 'null');
  } catch {
    return null;
  }
}

function formatRelativeTime(at) {
  if (!at) {
    return '';
  }
  const seconds = Math.max(0, Math.round((Date.now() - at) / 1000));
  if (seconds < 60) {
    return `${seconds}s ago`;
  }
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) {
    return `${minutes}m ago`;
  }
  const hours = Math.round(minutes / 60);
  if (hours < 24) {
    return `${hours}h ago`;
  }
  return `${Math.round(hours / 24)}d ago`;
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
