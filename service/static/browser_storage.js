const DB_NAME = 'qumirdb-browser-storage';
const DB_VERSION = 1;
const DATASET_STORE = 'datasets';

let dbPromise = null;

function openDb() {
  if (dbPromise) {
    return dbPromise;
  }
  dbPromise = new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, DB_VERSION);
    request.onupgradeneeded = () => {
      const db = request.result;
      if (!db.objectStoreNames.contains(DATASET_STORE)) {
        db.createObjectStore(DATASET_STORE, { keyPath: 'id' });
      }
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
  return dbPromise;
}

async function withStore(mode, fn) {
  const db = await openDb();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(DATASET_STORE, mode);
    const store = tx.objectStore(DATASET_STORE);
    const result = fn(store);
    tx.oncomplete = () => resolve(result);
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error);
  });
}

function requestResult(request) {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

async function datasetsDir(create = true) {
  if (!navigator.storage?.getDirectory) {
    throw new Error('OPFS is not available in this browser');
  }
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle('datasets', { create });
}

async function writeOpfsFile(datasetId, file) {
  const root = await datasetsDir(true);
  const dir = await root.getDirectoryHandle(datasetId, { create: true });
  const handle = await dir.getFileHandle(file.name, { create: true });
  const writable = await handle.createWritable();
  await writable.write(file);
  await writable.close();
  return `datasets/${datasetId}/${file.name}`;
}

export async function writeOpfsStoredFile(datasetId, file) {
  return {
    name: file.name,
    size: file.size,
    type: file.type || 'application/octet-stream',
    opfsPath: await writeOpfsFile(datasetId, file)
  };
}

export async function writeOpfsFileStream(datasetId, fileName, stream, options = {}) {
  const root = await datasetsDir(true);
  const dir = await root.getDirectoryHandle(datasetId, { create: true });
  const handle = await dir.getFileHandle(fileName, { create: true });
  const writable = await handle.createWritable();
  const reader = stream.getReader();
  let size = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) {
        break;
      }
      await writable.write(value);
      size += value.byteLength;
      options.onProgress?.(size);
    }
    await writable.close();
  } catch (error) {
    try {
      await writable.abort?.();
    } catch {
      // The original error is more useful.
    }
    throw error;
  }
  return {
    name: fileName,
    size,
    type: options.type || 'application/octet-stream',
    opfsPath: `datasets/${datasetId}/${fileName}`
  };
}

export async function readOpfsFile(datasetId, fileName) {
  const root = await datasetsDir(false);
  const dir = await root.getDirectoryHandle(datasetId, { create: false });
  const handle = await dir.getFileHandle(fileName, { create: false });
  return handle.getFile();
}

async function removeOpfsFile(datasetId, fileName) {
  try {
    const root = await datasetsDir(false);
    const dir = await root.getDirectoryHandle(datasetId, { create: false });
    await dir.removeEntry(fileName);
  } catch {
    // Metadata deletion is authoritative; OPFS cleanup is best effort.
  }
}

function tableNameFromFile(fileName) {
  return fileName
    .replace(/[.]parquet$/i, '')
    .replace(/[^A-Za-z0-9_]+/g, '_')
    .replace(/^_+|_+$/g, '') || 'table';
}

export async function listBrowserDatasets() {
  return withStore('readonly', store =>
    requestResult(store.getAll())
  );
}

export async function saveBrowserDataset(dataset) {
  await withStore('readwrite', store => {
    store.put(dataset);
  });
}

export async function deleteBrowserDataset(datasetId) {
  await withStore('readwrite', store => {
    store.delete(datasetId);
  });

  try {
    const root = await datasetsDir(false);
    await root.removeEntry(datasetId, { recursive: true });
  } catch {
    // Metadata deletion is authoritative; OPFS cleanup is best effort.
  }
}

export async function createBrowserDataset(name) {
  const id = crypto.randomUUID();
  const dataset = {
    id,
    name,
    source: {
      kind: 'browser',
      storage: 'opfs',
      files: []
    },
    tables: []
  };
  await saveBrowserDataset(dataset);
  return dataset;
}

export async function renameBrowserDataset(dataset, name) {
  const updated = {
    ...dataset,
    name
  };
  await saveBrowserDataset(updated);
  return updated;
}

export async function addFilesToBrowserDataset(dataset, files, tables) {
  const storedFiles = [];
  for (const file of files) {
    storedFiles.push(await writeOpfsStoredFile(dataset.id, file));
  }
  return addStoredFilesToBrowserDataset(dataset, storedFiles, tables);
}

export async function addStoredFilesToBrowserDataset(dataset, storedFiles, tables) {
  const fileNames = new Set(storedFiles.map(file => file.name));
  const tableItems = tables.map(table => ({
    ...table,
    sourceFile: table.sourceFile || `${table.name}.parquet`
  }));
  const tableSourceFiles = new Set(tableItems.map(table => table.sourceFile));
  const tableNames = new Set(tableItems.map(table => table.name));
  const source = dataset.source || {};
  const updated = {
    ...dataset,
    source: {
      ...source,
      kind: 'browser',
      storage: 'opfs',
      files: [
        ...(source.files || []).filter(file => !fileNames.has(file.name)),
        ...storedFiles
      ]
    },
    tables: [
      ...(dataset.tables || []).filter(table => {
        const sourceFile = table.sourceFile || `${table.name}.parquet`;
        return !tableSourceFiles.has(sourceFile) && !tableNames.has(table.name);
      }),
      ...tableItems
    ]
  };
  await saveBrowserDataset(updated);
  return updated;
}

export async function removeFileFromBrowserDataset(dataset, fileName) {
  await removeOpfsFile(dataset.id, fileName);

  const tableName = tableNameFromFile(fileName);
  const source = dataset.source || {};
  const updated = {
    ...dataset,
    source: {
      ...source,
      files: (source.files || []).filter(file => file.name !== fileName)
    },
    tables: (dataset.tables || []).filter(table => {
      const sourceFile = table.sourceFile || `${table.name}.parquet`;
      return sourceFile !== fileName && table.name !== tableName;
    })
  };
  await saveBrowserDataset(updated);
  return updated;
}
