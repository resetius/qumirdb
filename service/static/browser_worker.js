import { readOpfsFile } from './browser_storage.js';
import { readParquetColumnBatches } from './browser_parquet.js';
import { executeBrowserPipelineScheduled } from './browser_runtime.js';

self.onmessage = async event => {
  const message = event.data || {};
  if (message.type !== 'run') {
    return;
  }

  try {
    const result = await executeBrowserPipelineScheduled(
      message.exec,
      (stage, onProgress) => readBrowserSourceBatches(
        message.dataset, stage, onProgress),
      {
        onProgress(progress) {
          self.postMessage({ type: 'progress', progress });
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

async function* readBrowserSourceBatches(dataset, stage, onProgress) {
  const table = (dataset.tables || []).find(item => item.name === stage.table);
  if (!table) {
    throw new Error(`table not found in dataset: ${stage.table}`);
  }
  const fileName = table.sourceFile || `${table.name}.parquet`;
  const file = await readOpfsFile(dataset.id, fileName);
  for await (const batch of readParquetColumnBatches(file, stage.columns, { onProgress })) {
    yield {
      rowCount: batch.rowCount,
      columns: stage.columns.map(column => ({
        name: column.name,
        type: column.type,
        values: batch.columns[column.name] || []
      }))
    };
  }
}
