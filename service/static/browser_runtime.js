// Browser execution runtime for simple one-to-one pipelines.
//
// Drives the standalone WASM kernels emitted by qdb_plan_export (bundle.exec):
// marshals a columnar batch into a kernel module's linear memory as
// TColumn[]/TRowSet per the verified 8-byte-pointer layout, runs the kernel, and
// reads results back. See PLAN_BROWSER_EXECUTION.md for the ABI/layout facts.

const PAGE = 65536;

function alignUp(value, align) {
  return (value + (align - 1)) & ~(align - 1);
}

function base64ToBytes(base64) {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; ++i) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

// Byte width of a fixed-width core type in a kernel data buffer.
export function coreTypeWidth(type) {
  switch (type) {
    case 'i8': case 'u8': case 'bool': return 1;
    case 'i16': case 'u16': return 2;
    case 'i32': case 'u32': return 4;
    case 'i64': case 'u64': case 'f64': return 8;
    case 'string': return 16; // StringView (ptr + size)
    default: return 0;
  }
}

function isBigType(type) {
  return type === 'i64' || type === 'u64';
}

// A bump allocator over one kernel module's linear memory. All allocations are
// performed before any writes so a mid-write memory.grow() never detaches a view.
class Arena {
  constructor(instance) {
    this.memory = instance.exports.memory;
    const heapBase = instance.exports.__heap_base
      ? Number(instance.exports.__heap_base.value)
      : PAGE;
    this.offset = alignUp(heapBase, 16);
  }

  alloc(bytes, align = 8) {
    this.offset = alignUp(this.offset, align);
    const ptr = this.offset;
    this.offset += bytes;
    const need = this.offset;
    const have = this.memory.buffer.byteLength;
    if (need > have) {
      this.memory.grow(Math.ceil((need - have) / PAGE));
    }
    return ptr;
  }

  view() {
    return new DataView(this.memory.buffer);
  }

  bytes() {
    return new Uint8Array(this.memory.buffer);
  }
}

function writePointer(dv, offset, address) {
  // Pointers are 8-byte fields; the address lives in the low 32 bits.
  dv.setUint32(offset, address >>> 0, true);
  dv.setUint32(offset + 4, 0, true);
}

function writeNumericValue(dv, address, type, value) {
  switch (type) {
    case 'i8': dv.setInt8(address, Number(value) | 0); break;
    case 'u8': case 'bool': dv.setUint8(address, Number(value) & 0xff); break;
    case 'i16': dv.setInt16(address, Number(value) | 0, true); break;
    case 'u16': dv.setUint16(address, Number(value) & 0xffff, true); break;
    case 'i32': dv.setInt32(address, Number(value) | 0, true); break;
    case 'u32': dv.setUint32(address, Number(value) >>> 0, true); break;
    case 'i64': dv.setBigInt64(address, BigInt(value ?? 0), true); break;
    case 'u64': dv.setBigUint64(address, BigInt(value ?? 0), true); break;
    case 'f64': dv.setFloat64(address, Number(value ?? 0), true); break;
    default: throw new Error(`unsupported numeric column type: ${type}`);
  }
}

function readNumericValue(dv, address, type) {
  switch (type) {
    case 'i8': return dv.getInt8(address);
    case 'u8': case 'bool': return dv.getUint8(address);
    case 'i16': return dv.getInt16(address, true);
    case 'u16': return dv.getUint16(address, true);
    case 'i32': return dv.getInt32(address, true);
    case 'u32': return dv.getUint32(address, true);
    case 'i64': return dv.getBigInt64(address, true);
    case 'u64': return dv.getBigUint64(address, true);
    case 'f64': return dv.getFloat64(address, true);
    default: throw new Error(`unsupported numeric column type: ${type}`);
  }
}

// Compile + instantiate a kernel module. The numeric MVP kernels have no
// imports; anything that does (string/date externs) is rejected up front.
export async function instantiateKernel(base64, entryName) {
  const module = await WebAssembly.compile(base64ToBytes(base64));
  const imports = WebAssembly.Module.imports(module);
  if (imports.length > 0) {
    const names = imports.map(item => item.name).join(', ');
    throw new Error(`kernel needs unsupported imports: ${names}`);
  }
  const instance = await WebAssembly.instantiate(module, {});
  if (instance.exports.__wasm_call_ctors) {
    instance.exports.__wasm_call_ctors();
  }
  const fn = instance.exports[entryName];
  if (typeof fn !== 'function') {
    throw new Error(`kernel is missing entry ${entryName}`);
  }
  return { instance, fn };
}

// Marshal `columns` (source column order) into a fresh TRowSet inside `arena`.
// Numeric columns get a real data buffer; string columns (passthrough only in
// the MVP) get a zeroed scratch so any dead kernel read stays in-bounds.
function marshalRowSet(arena, layout, columns, rowCount, wantSelection) {
  const colLayout = layout.column;
  const rsLayout = layout.rowset;

  const columnsBase = arena.alloc(columns.length * colLayout.size, 8);
  const dataPtrs = new Array(columns.length).fill(0);
  const scratch = arena.alloc(16, 8);

  for (let c = 0; c < columns.length; ++c) {
    const column = columns[c];
    if (column.type === 'string') {
      dataPtrs[c] = scratch;
    } else {
      const width = coreTypeWidth(column.type);
      dataPtrs[c] = arena.alloc(Math.max(rowCount, 1) * width, 8);
    }
  }

  const selectionPtr = wantSelection ? arena.alloc(Math.max(rowCount, 1), 8) : 0;
  const rowsetPtr = arena.alloc(rsLayout.size, 8);

  // All allocations done; safe to take a view and write.
  const dv = arena.view();

  for (let c = 0; c < columns.length; ++c) {
    const column = columns[c];
    const colPtr = columnsBase + c * colLayout.size;
    new Uint8Array(arena.memory.buffer, colPtr, colLayout.size).fill(0);
    writePointer(dv, colPtr + colLayout.data, dataPtrs[c]);
    if (column.type !== 'string') {
      const width = coreTypeWidth(column.type);
      const values = column.values;
      for (let i = 0; i < rowCount; ++i) {
        writeNumericValue(dv, dataPtrs[c] + i * width, column.type, values[i]);
      }
    }
  }

  new Uint8Array(arena.memory.buffer, rowsetPtr, rsLayout.size).fill(0);
  writePointer(dv, rowsetPtr + rsLayout.columns, columnsBase);
  dv.setBigInt64(rowsetPtr + rsLayout.columnCount, BigInt(columns.length), true);
  dv.setBigInt64(rowsetPtr + rsLayout.rowCount, BigInt(rowCount), true);
  if (wantSelection) {
    writePointer(dv, rowsetPtr + rsLayout.selection, selectionPtr);
  }

  return { rowsetPtr, selectionPtr, columnsBase };
}

// Run a filter kernel over `batch` (columns in source order). Returns a
// Uint8Array selection where a nonzero byte marks a kept row.
export function runFilter(kernel, layout, batch) {
  const arena = new Arena(kernel.instance);
  const { rowsetPtr, selectionPtr } =
    marshalRowSet(arena, layout, batch.columns, batch.rowCount, true);
  kernel.fn(rowsetPtr);
  // Copy the selection out before the buffer can be reused.
  return arena.bytes().slice(selectionPtr, selectionPtr + batch.rowCount);
}

// Run a project kernel over `batch`; `output` is exec.stages[project].output.
// Returns the new columns array (source order preserved for the pipeline).
export function runProject(kernel, layout, batch, output) {
  const arena = new Arena(kernel.instance);
  const rowCount = batch.rowCount;

  const computed = output.filter(col => col.source === 'computed');
  const computedBufs = computed.map(col =>
    arena.alloc(Math.max(rowCount, 1) * col.width, 8));
  const outArrayPtr = computed.length
    ? arena.alloc(computed.length * 8, 8)
    : 0;

  const { rowsetPtr } =
    marshalRowSet(arena, layout, batch.columns, rowCount, false);

  const dv = arena.view();
  for (let k = 0; k < computed.length; ++k) {
    writePointer(dv, outArrayPtr + k * 8, computedBufs[k]);
  }

  if (computed.length > 0) {
    kernel.fn(rowsetPtr, outArrayPtr);
  }

  // Re-take the view: the kernel may have grown memory internally.
  const outDv = arena.view();
  const columns = [];
  for (const col of output) {
    if (col.source === 'passthrough') {
      columns.push({
        name: col.name,
        type: col.type,
        values: batch.columns[col.inputIndex].values,
      });
    } else {
      const bufPtr = computedBufs[computed.indexOf(col)];
      const values = new Array(rowCount);
      if (col.isString) {
        const sv = layout.stringView;
        const memBytes = new Uint8Array(kernel.instance.exports.memory.buffer);
        for (let i = 0; i < rowCount; ++i) {
          const base = bufPtr + i * sv.size;
          const dataPtr = outDv.getUint32(base + sv.data, true);
          const size = Number(outDv.getBigInt64(base + sv.length, true));
          values[i] = new TextDecoder().decode(
            memBytes.subarray(dataPtr, dataPtr + size));
        }
      } else {
        for (let i = 0; i < rowCount; ++i) {
          values[i] = readNumericValue(outDv, bufPtr + i * col.width, col.type);
        }
      }
      columns.push({ name: col.name, type: col.type, values });
    }
  }
  return columns;
}

// Execute a linear exec pipeline. `readSource(stage)` must return
// { rowCount, columns: [{ name, type, values }] } in the stage's column order.
export async function executeBrowserPipeline(exec, readSource) {
  if (!exec || exec.supported !== true) {
    throw new Error(exec?.reason || 'pipeline is not supported for browser execution');
  }
  if (exec.embedWasm !== true) {
    throw new Error('exec plan was produced without embedded wasm kernels');
  }

  const layout = exec.layout;
  let batch = null;
  let selection = null;
  let limit = null;

  for (const stage of exec.stages) {
    if (stage.kind === 'source') {
      batch = await readSource(stage);
      selection = null;
    } else if (stage.kind === 'filter') {
      const kernel = await instantiateKernel(stage.wasm, '<kernel>');
      selection = runFilter(kernel, layout, batch);
    } else if (stage.kind === 'project') {
      let columns;
      if (stage.wasm) {
        const kernel = await instantiateKernel(stage.wasm, '<project>');
        columns = runProject(kernel, layout, batch, stage.output);
      } else {
        // Pure passthrough project: no kernel, remap columns directly.
        columns = stage.output.map(col => ({
          name: col.name,
          type: col.type,
          values: batch.columns[col.inputIndex].values,
        }));
      }
      batch = { rowCount: batch.rowCount, columns };
    } else if (stage.kind === 'sort') {
      batch = applySort(batch, selection, stage.keys, null);
      selection = null;
    } else if (stage.kind === 'top-sort') {
      batch = applySort(batch, selection, stage.keys, Number(stage.limit));
      selection = null;
    } else if (stage.kind === 'limit') {
      limit = { limit: Number(stage.limit), offset: Number(stage.offset || 0) };
    } else {
      throw new Error(`unsupported exec stage: ${stage.kind}`);
    }
  }

  return materialize(batch, selection, limit);
}

// Sort a batch by `keys` (each { index, direction, nulls }), folding any
// pending selection into a compacted, reordered batch. When `limit` is set
// (top-sort), only the first `limit` rows are kept. Returns a new batch with no
// selection; downstream stages see fully materialized ordered rows.
function applySort(batch, selection, keys, limit) {
  const indices = [];
  for (let i = 0; i < batch.rowCount; ++i) {
    if (selection && !selection[i]) {
      continue;
    }
    indices.push(i);
  }

  indices.sort((a, b) => {
    for (const key of keys) {
      const values = batch.columns[key.index].values;
      const c = compareByKey(values[a], values[b], key);
      if (c !== 0) {
        return c;
      }
    }
    return 0;
  });

  const keep = (limit != null && limit < indices.length) ? limit : indices.length;
  const kept = indices.slice(0, Math.max(keep, 0));
  const columns = batch.columns.map(col => ({
    name: col.name,
    type: col.type,
    values: kept.map(idx => col.values[idx]),
  }));
  return { rowCount: kept.length, columns };
}

// Compare two column values under one sort key, returning final order
// (direction and null placement already applied). Null default follows the
// Postgres convention: NULLS LAST for asc, NULLS FIRST for desc.
function compareByKey(a, b, key) {
  const aNull = a === null || a === undefined;
  const bNull = b === null || b === undefined;
  if (aNull || bNull) {
    if (aNull && bNull) {
      return 0;
    }
    let nullsFirst;
    if (key.nulls === 'nulls-first') {
      nullsFirst = true;
    } else if (key.nulls === 'nulls-last') {
      nullsFirst = false;
    } else {
      nullsFirst = key.direction === 'desc';
    }
    return aNull === nullsFirst ? -1 : 1;
  }

  let c = 0;
  if (a < b) {
    c = -1;
  } else if (a > b) {
    c = 1;
  }
  return key.direction === 'desc' ? -c : c;
}

// Apply selection + limit and produce { columns: string[], rows: any[][] }.
function materialize(batch, selection, limit) {
  const columns = batch.columns.map(col => col.name);
  const rows = [];
  const offset = limit ? limit.offset : 0;
  const max = limit ? limit.limit : Infinity;
  let skipped = 0;

  for (let i = 0; i < batch.rowCount; ++i) {
    if (selection && !selection[i]) {
      continue;
    }
    if (skipped < offset) {
      ++skipped;
      continue;
    }
    if (rows.length >= max) {
      break;
    }
    rows.push(batch.columns.map(col => formatValue(col.values[i])));
  }
  return { columns, rows };
}

function formatValue(value) {
  if (value === null || value === undefined) {
    return '';
  }
  if (typeof value === 'bigint') {
    return value.toString();
  }
  return value;
}
