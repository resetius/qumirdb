import { readOpfsFile } from './browser_storage.js';
import { readParquetWasmColumnBatches } from './browser_parquet.js';
import { executeBrowserPipelineScheduled } from './browser_runtime.js';

self.onmessage = async event => {
  const message = event.data || {};
  if (message.type !== 'run') {
    return;
  }

  try {
    const progress = createProgressTracker(message.exec, message.dataset);
    const result = await executeBrowserPipelineScheduled(
      message.exec,
      (stage, onProgress, runtime) => readBrowserSourceBatches(
        message.dataset, stage, progress.wrap(stage, onProgress), runtime),
      {
        onProgress(update) {
          self.postMessage({ type: 'progress', progress: update });
        }
      });
    self.postMessage({ type: 'result', result });
  } catch (error) {
    self.postMessage({
      type: 'error',
      error: {
        message: error.message || String(error),
        stack: error.stack || '',
      },
    });
  }
};

function createProgressTracker(exec, dataset) {
  if (!Array.isArray(exec.nodes)) {
    throw new Error('exec plan is missing graph nodes');
  }
  const sources = exec.nodes.filter(node => node.kind === 'source');
  const items = sources.map((stage, index) => {
    const key = String(stage.id ?? `${stage.table}:${index}`);
    return {
      key,
      total: sourceTotalUnits(dataset, stage),
      processed: 0,
    };
  });
  const byKey = new Map(items.map(item => [item.key, item]));

  const sourceKey = stage => String(stage.id ?? `${stage.table}:0`);

  const publish = (onProgress) => {
    const completedUnits = items.reduce(
      (sum, item) => sum + Math.min(item.processed, item.total), 0);
    const totalUnits = Math.max(
      items.reduce((sum, item) => sum + item.total, 0),
      1);
    onProgress({
      phase: 'read',
      completedUnits,
      totalUnits,
      sourceCount: items.length,
    });
  };

  return {
    wrap(stage, onProgress) {
      const key = sourceKey(stage);
      const item = byKey.get(key);
      if (!item) {
        return update => onProgress(update);
      }
      publish(onProgress);
      return update => {
        if (update?.totalUnits) {
          item.total = Number(update.totalUnits);
        }
        if (update?.completedUnits != null) {
          item.processed = Number(update.completedUnits);
        }
        publish(onProgress);
      };
    }
  };
}

function sourceTotalUnits(dataset, stage) {
  const table = (dataset.tables || []).find(item => item.name === stage.table);
  const rows = Number(table?.stats?.rows || 0);
  const columns = Math.max((stage.columns || []).length, 1);
  return Math.max(rows * columns, 1);
}

async function* readBrowserSourceBatches(dataset, stage, onProgress, runtime) {
  if (!runtime?.shared?.arena || !runtime?.layout) {
    throw new Error('browser source needs shared wasm runtime context');
  }
  const table = (dataset.tables || []).find(item => item.name === stage.table);
  if (!table) {
    throw new Error(`table not found in dataset: ${stage.table}`);
  }
  const fileName = table.sourceFile || `${table.name}.parquet`;
  const file = await readOpfsFile(dataset.id, fileName);
  for await (const batch of readParquetWasmColumnBatches(file, stage.columns, {
    onProgress,
    arena: runtime.shared.arena,
    layout: runtime.layout,
  })) {
    yield batch;
  }
}
