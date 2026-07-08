const HYPARQUET_URL = 'https://cdn.jsdelivr.net/npm/hyparquet@latest/+esm';
const HYPARQUET_COMPRESSORS_URL =
  'https://cdn.jsdelivr.net/npm/hyparquet-compressors@latest/+esm';

let hyparquetPromise = null;
let compressorsPromise = null;

async function loadHyparquet() {
  if (!hyparquetPromise) {
    hyparquetPromise = import(HYPARQUET_URL);
  }
  return hyparquetPromise;
}

async function loadCompressors() {
  if (!compressorsPromise) {
    compressorsPromise = import(HYPARQUET_COMPRESSORS_URL)
      .then(module => module.compressors)
      .catch(() => undefined);
  }
  return compressorsPromise;
}

function asyncBufferFromBlob(file) {
  return {
    byteLength: file.size,
    async slice(start, end) {
      return file.slice(start, end).arrayBuffer();
    }
  };
}

async function readMetadata(file) {
  const parquet = await loadHyparquet();
  if (parquet.parquetMetadataAsync) {
    const asyncBuffer = parquet.asyncBufferFromFile
      ? await parquet.asyncBufferFromFile(file)
      : asyncBufferFromBlob(file);
    return parquet.parquetMetadataAsync(asyncBuffer);
  }
  if (parquet.parquetMetadata) {
    return parquet.parquetMetadata(await file.arrayBuffer());
  }
  throw new Error('hyparquet metadata API is not available');
}

function tableNameFromFile(fileName) {
  return fileName
    .replace(/[.]parquet$/i, '')
    .replace(/[^A-Za-z0-9_]+/g, '_')
    .replace(/^_+|_+$/g, '') || 'table';
}

function parquetTypeName(field) {
  const physical = String(parquetPhysicalType(field.type ?? field.physicalType)).toUpperCase();
  const converted = String(parquetConvertedType(
    field.converted_type ?? field.convertedType)).toUpperCase();
  const logical = JSON.stringify(field.logicalType || field.logical_type || {}).toUpperCase();

  if (converted.includes('UTF8') || logical.includes('STRING')) {
    return 'string';
  }
  if (logical.includes('DATE')) {
    return 'i32';
  }
  if (logical.includes('TIMESTAMP')) {
    return 'i64';
  }
  if (physical === 'BOOLEAN') {
    return 'bool';
  }
  if (physical === 'INT32') {
    return 'i32';
  }
  if (physical === 'INT64') {
    return 'i64';
  }
  if (physical === 'FLOAT' || physical === 'DOUBLE') {
    return 'f64';
  }
  if (physical === 'BYTE_ARRAY' || physical === 'FIXED_LEN_BYTE_ARRAY') {
    return 'string';
  }
  return 'string';
}

function parquetPhysicalType(type) {
  if (typeof type !== 'number') {
    return type || '';
  }
  return [
    'BOOLEAN',
    'INT32',
    'INT64',
    'INT96',
    'FLOAT',
    'DOUBLE',
    'BYTE_ARRAY',
    'FIXED_LEN_BYTE_ARRAY'
  ][type] || '';
}

function parquetConvertedType(type) {
  if (typeof type !== 'number') {
    return type || '';
  }
  return [
    'UTF8',
    'MAP',
    'MAP_KEY_VALUE',
    'LIST',
    'ENUM',
    'DECIMAL',
    'DATE',
    'TIME_MILLIS',
    'TIME_MICROS',
    'TIMESTAMP_MILLIS',
    'TIMESTAMP_MICROS',
    'UINT_8',
    'UINT_16',
    'UINT_32',
    'UINT_64',
    'INT_8',
    'INT_16',
    'INT_32',
    'INT_64',
    'JSON',
    'BSON',
    'INTERVAL'
  ][type] || '';
}

function schemaFields(metadata) {
  const schema = metadata.schema || metadata.schemaElements || metadata.schema_elements || [];
  if (!Array.isArray(schema)) {
    throw new Error('parquet schema is not an array');
  }
  return schema
    .filter(field => field?.name)
    .filter(field => {
      const children = field.num_children ?? field.numChildren ?? 0;
      return children === 0;
    })
    .filter((field, index) => index > 0 || field.type || field.physicalType)
    .map(field => ({
      name: field.name,
      type: parquetTypeName(field)
    }));
}

function rowGroupStats(metadata) {
  const rowGroups = metadata.row_groups || metadata.rowGroups || [];
  const rows = Number(metadata.num_rows ?? metadata.numRows ?? 0);
  const bytes = rowGroups.reduce((sum, group) => {
    return sum + Number(group.total_byte_size ?? group.totalByteSize ?? 0);
  }, 0);
  return {
    rows,
    rowGroups: rowGroups.length || 1,
    bytes
  };
}

export async function readParquetTable(file) {
  const metadata = await readMetadata(file);
  return {
    name: tableNameFromFile(file.name),
    columns: schemaFields(metadata),
    stats: rowGroupStats(metadata)
  };
}

// Reads the requested columns as raw column arrays keyed by column name.
// Numeric chunks stay as TypedArrays when hyparquet provides them.
export async function readParquetColumns(file, requestedColumns) {
  const specs = requestedColumns.map(column =>
    typeof column === 'string' ? { name: column } : column);
  const columnNames = specs.map(column => column.name);
  const parquet = await loadHyparquet();
  const asyncBuffer = parquet.asyncBufferFromFile
    ? await parquet.asyncBufferFromFile(file)
    : asyncBufferFromBlob(file);
  const compressors = await loadCompressors();

  if (typeof parquet.parquetRead !== 'function') {
    throw new Error('hyparquet parquetRead API is not available');
  }

  if (columnNames.length === 0) {
    const metadata = await readMetadata(file);
    return {
      rowCount: Number(metadata.num_rows ?? metadata.numRows ?? 0),
      columns: {},
    };
  }

  const chunks = Object.fromEntries(columnNames.map(name => [name, []]));
  let rowCount = 0;
  await parquet.parquetRead({
    file: asyncBuffer,
    columns: columnNames,
    compressors,
    onChunk({ columnName, columnData, rowStart, rowEnd }) {
      const name = String(columnName);
      const columnChunks = chunks[name];
      if (!columnChunks) return;

      const start = Number(rowStart);
      const end = Number(rowEnd);
      rowCount = Math.max(rowCount, end);
      columnChunks.push({ start, end, data: columnData });
    },
  });
  const columns = {};
  for (const spec of specs) {
    columns[spec.name] = assembleColumn(chunks[spec.name], rowCount);
  }
  return { rowCount, columns };
}

function assembleColumn(chunks, rowCount) {
  if (!chunks || chunks.length === 0) return [];
  if (chunks.length === 1 && chunks[0].start === 0 && chunks[0].end === rowCount) {
    return chunks[0].data;
  }

  const first = chunks[0].data;
  if (ArrayBuffer.isView(first) && typeof first.BYTES_PER_ELEMENT === 'number') {
    const out = new first.constructor(rowCount);
    for (const chunk of chunks) {
      out.set(chunk.data, chunk.start);
    }
    return out;
  }

  const out = new Array(rowCount);
  for (const chunk of chunks) {
    for (let i = chunk.start; i < chunk.end; ++i) {
      out[i] = chunk.data[i - chunk.start];
    }
  }
  return out;
}
