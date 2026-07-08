// Browser execution runtime for simple one-to-one pipelines.
//
// Drives the standalone WASM kernels emitted by qdb_plan_export (bundle.exec):
// marshals a columnar batch into a kernel module's linear memory as
// TColumn[]/TRowSet per the verified 8-byte-pointer layout, runs the kernel, and
// reads results back. See PLAN_BROWSER_EXECUTION.md for the ABI/layout facts.

const PAGE = 65536;
const EMPTY_BYTES = new Uint8Array(0);

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
      const pages = Math.ceil((need - have) / PAGE);
      // memory64 engines may require a BigInt page delta.
      try {
        this.memory.grow(pages);
      } catch {
        this.memory.grow(BigInt(pages));
      }
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

// memcmp-style byte comparison matching qdb_filter_string_compare: -1/0/1 with
// length as tiebreak.
function compareBytes(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; ++i) {
    if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return a.length === b.length ? 0 : (a.length < b.length ? -1 : 1);
}

// SQL LIKE over bytes (case-sensitive, % = any run, _ = one byte, no escape),
// matching qdb_string_view_sql_like. Latin1 keeps one byte == one char.
function sqlLikeBytes(value, pattern) {
  let re = '^';
  for (const b of pattern) {
    const ch = String.fromCharCode(b);
    if (ch === '%') re += '.*';
    else if (ch === '_') re += '.';
    else re += ch.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  }
  re += '$';
  let s = '';
  for (const b of value) s += String.fromCharCode(b);
  return new RegExp(re, 's').test(s) ? 1 : 0;
}

// The `env` imports the string/predicate kernels need. Pointer + size args are
// i64 (BigInt under memory64); bool results are i32 (0/1), compare/like are i64.
// `getMemory` is read lazily since the instance doesn't exist yet at build time.
function createQdbEnv(getMemory) {
  const bytesAt = (ptr, size) => {
    const mem = new Uint8Array(getMemory().buffer);
    const start = Number(ptr);
    return mem.subarray(start, start + Number(size));
  };
  const cstrAt = (ptr) => {
    const mem = new Uint8Array(getMemory().buffer);
    let start = Number(ptr);
    let end = start;
    while (mem[end] !== 0) ++end;
    return mem.subarray(start, end);
  };

  const svLit = (t) => (d, s, lit) => (t(compareBytes(bytesAt(d, s), cstrAt(lit))) ? 1 : 0);
  const litSv = (t) => (lit, d, s) => (t(compareBytes(cstrAt(lit), bytesAt(d, s))) ? 1 : 0);
  const litLit = (t) => (l, r) => (t(compareBytes(cstrAt(l), cstrAt(r))) ? 1 : 0);
  const EQ = c => c === 0, NE = c => c !== 0;
  const LT = c => c < 0, LE = c => c <= 0, GT = c => c > 0, GE = c => c >= 0;

  const norm = v => (v < 0n ? 1n : v);
  return {
    qdb_filter_string_compare: (ld, ls, rd, rs) =>
      BigInt(compareBytes(bytesAt(ld, ls), bytesAt(rd, rs))),
    qdb_string_view_cmp_cstr: (d, s, c) => BigInt(compareBytes(bytesAt(d, s), cstrAt(c))),
    qdb_cstr_cmp_cstr: (a, b) => BigInt(compareBytes(cstrAt(a), cstrAt(b))),
    qdb_string_view_sql_like: (d, s, p) => BigInt(sqlLikeBytes(bytesAt(d, s), cstrAt(p))),

    qdb_sv_lit_eq: svLit(EQ), qdb_sv_lit_ne: svLit(NE), qdb_sv_lit_lt: svLit(LT),
    qdb_sv_lit_le: svLit(LE), qdb_sv_lit_gt: svLit(GT), qdb_sv_lit_ge: svLit(GE),
    qdb_lit_sv_eq: litSv(EQ), qdb_lit_sv_ne: litSv(NE), qdb_lit_sv_lt: litSv(LT),
    qdb_lit_sv_le: litSv(LE), qdb_lit_sv_gt: litSv(GT), qdb_lit_sv_ge: litSv(GE),
    qdb_lit_lit_eq: litLit(EQ), qdb_lit_lit_ne: litLit(NE), qdb_lit_lit_lt: litLit(LT),
    qdb_lit_lit_le: litLit(LE), qdb_lit_lit_gt: litLit(GT), qdb_lit_lit_ge: litLit(GE),

    // SQL three-valued booleans (0 false, 1 true, 2 unknown); negatives => true.
    qdb_sql_bool_and: (l, r) => {
      l = norm(l); r = norm(r);
      if (l === 0n || r === 0n) return 0n;
      return (l === 1n && r === 1n) ? 1n : 2n;
    },
    qdb_sql_bool_or: (l, r) => {
      l = norm(l); r = norm(r);
      if (l === 1n || r === 1n) return 1n;
      return (l === 0n && r === 0n) ? 0n : 2n;
    },
    qdb_sql_bool_not: (v) => {
      v = norm(v);
      return v === 2n ? 2n : 1n - v;
    },
  };
}

// Compile + instantiate a kernel module, wiring the `env` string runtime. Any
// import we don't implement is reported so the caller can fall back to server
// execution rather than run a partially-linked kernel.
export async function instantiateKernel(base64, entryName) {
  const module = await WebAssembly.compile(base64ToBytes(base64));
  const holder = { memory: null };
  const env = createQdbEnv(() => holder.memory);
  for (const imp of WebAssembly.Module.imports(module)) {
    if (imp.module !== 'env' || typeof env[imp.name] !== 'function') {
      throw new Error(`kernel needs unsupported import: ${imp.module}.${imp.name}`);
    }
  }
  const instance = await WebAssembly.instantiate(module, { env });
  holder.memory = instance.exports.memory;
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
// Numeric columns get a data buffer; string columns get a concatenated UTF-8
// bytes buffer plus an i32 offsets array (OffsetWidth = 4), matching the layout
// the kernel reads (Data + Offsets[row]).
function marshalRowSet(arena, layout, columns, rowCount, wantSelection) {
  const colLayout = layout.column;
  const rsLayout = layout.rowset;
  const encoder = new TextEncoder();

  // Pre-encode string columns before any allocation (a mid-write grow would
  // detach views): parts hold each row's bytes, offsets are cumulative.
  const encoded = columns.map(column => {
    if (column.type !== 'string') return null;
    const parts = new Array(rowCount);
    const offsets = new Int32Array(rowCount + 1);
    let total = 0;
    for (let i = 0; i < rowCount; ++i) {
      const value = column.values[i];
      const b = (value === null || value === undefined)
        ? EMPTY_BYTES
        : encoder.encode(String(value));
      parts[i] = b;
      offsets[i] = total;
      total += b.length;
    }
    offsets[rowCount] = total;
    return { parts, offsets, total };
  });

  const columnsBase = arena.alloc(columns.length * colLayout.size, 8);
  const dataPtrs = new Array(columns.length).fill(0);
  const offsetsPtrs = new Array(columns.length).fill(0);

  for (let c = 0; c < columns.length; ++c) {
    const column = columns[c];
    if (column.type === 'string') {
      dataPtrs[c] = arena.alloc(Math.max(encoded[c].total, 1), 1);
      offsetsPtrs[c] = arena.alloc((rowCount + 1) * 4, 4);
    } else {
      const width = coreTypeWidth(column.type);
      dataPtrs[c] = arena.alloc(Math.max(rowCount, 1) * width, 8);
    }
  }

  const selectionPtr = wantSelection ? arena.alloc(Math.max(rowCount, 1), 8) : 0;
  const rowsetPtr = arena.alloc(rsLayout.size, 8);

  // All allocations done; safe to take views and write.
  const dv = arena.view();
  const memBytes = arena.bytes();

  for (let c = 0; c < columns.length; ++c) {
    const column = columns[c];
    const colPtr = columnsBase + c * colLayout.size;
    new Uint8Array(arena.memory.buffer, colPtr, colLayout.size).fill(0);
    writePointer(dv, colPtr + colLayout.data, dataPtrs[c]);
    if (column.type === 'string') {
      const enc = encoded[c];
      let p = dataPtrs[c];
      for (let i = 0; i < rowCount; ++i) {
        memBytes.set(enc.parts[i], p);
        p += enc.parts[i].length;
      }
      for (let i = 0; i <= rowCount; ++i) {
        dv.setInt32(offsetsPtrs[c] + i * 4, enc.offsets[i], true);
      }
      writePointer(dv, colPtr + colLayout.offsets, offsetsPtrs[c]);
      dv.setUint8(colPtr + colLayout.offsetWidth, 4);
    } else {
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
  // memory64 entry: the rowset pointer is an i64 param.
  kernel.fn(BigInt(rowsetPtr));
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
    // memory64 entry: rowset and out-array pointers are i64 params.
    kernel.fn(BigInt(rowsetPtr), BigInt(outArrayPtr));
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

// Drive the radix composite sort kernel. Marshals each key column into a
// contiguous raw buffer, runs qdb_radix_sort_indices_composite to get a sorted
// permutation, then gathers all columns by it (top-`limit` if given). No
// ordering logic lives here — the kernel owns it, exactly as the native path.
export function runRadixSort(kernel, layout, batch, selection, radixKeys, limit) {
  const arena = new Arena(kernel.instance);

  const live = [];
  for (let i = 0; i < batch.rowCount; ++i) {
    if (selection && !selection[i]) continue;
    live.push(i);
  }
  const n = live.length;
  const keyCount = radixKeys.length;

  // Allocate everything up front (a mid-write grow would detach views).
  const valueBufs = radixKeys.map(key => {
    const width = coreTypeWidth(batch.columns[key.index].type);
    return { ptr: arena.alloc(Math.max(n, 1) * width, 8), width, key };
  });
  const valuesArrPtr = arena.alloc(Math.max(keyCount, 1) * 8, 8);
  const indicesPtr = arena.alloc(Math.max(n, 1) * 4, 4);
  const workPtr = arena.alloc(Math.max(n, 1) * 4, 4);
  const countsPtr = arena.alloc(256 * 4, 4);
  const descsPtr = arena.alloc(Math.max(keyCount, 1), 1);

  const dv = arena.view();
  for (let k = 0; k < keyCount; ++k) {
    const { ptr, width, key } = valueBufs[k];
    const column = batch.columns[key.index];
    for (let i = 0; i < n; ++i) {
      writeNumericValue(dv, ptr + i * width, column.type, column.values[live[i]]);
    }
    writePointer(dv, valuesArrPtr + k * 8, ptr);
    dv.setUint8(descsPtr + k, key.desc ? 1 : 0);
  }
  for (let i = 0; i < n; ++i) {
    dv.setUint32(indicesPtr + i * 4, i, true);
  }

  // memory64: every pointer/count is an i64 param.
  kernel.fn(
    BigInt(valuesArrPtr), BigInt(indicesPtr), BigInt(workPtr),
    BigInt(countsPtr), BigInt(n), BigInt(descsPtr));

  const outDv = arena.view();
  const order = new Array(n);
  for (let i = 0; i < n; ++i) {
    order[i] = live[outDv.getUint32(indicesPtr + i * 4, true)];
  }

  const keep = (limit != null && limit < n) ? Math.max(limit, 0) : n;
  const kept = order.slice(0, keep);
  const columns = batch.columns.map(col => ({
    name: col.name,
    type: col.type,
    values: kept.map(idx => col.values[idx]),
  }));
  return { rowCount: kept.length, columns };
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
    } else if (stage.kind === 'sort' || stage.kind === 'top-sort') {
      const rowLimit = stage.kind === 'top-sort' ? Number(stage.limit) : null;
      if (stage.wasm) {
        // Radix sort kernel: JS only marshals key bytes and gathers by the
        // returned permutation — all ordering happens in the kernel.
        const kernel =
          await instantiateKernel(stage.wasm, 'qdb_radix_sort_indices_composite');
        batch = runRadixSort(kernel, layout, batch, selection, stage.radixKeys, rowLimit);
      } else {
        // Non-radix keys (strings): comparison sort in JS.
        batch = applySort(batch, selection, stage.keys, rowLimit);
      }
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

  // Per-key comparison context. String keys are pre-encoded to UTF-8 once so
  // ordering matches the native host compare (byte order, not JS UTF-16).
  const encoder = new TextEncoder();
  const keyCtx = keys.map(key => {
    const column = batch.columns[key.index];
    if (column.type === 'string') {
      const bytes = column.values.map(v =>
        (v === null || v === undefined) ? null : encoder.encode(String(v)));
      return { key, isString: true, bytes };
    }
    return { key, isString: false, values: column.values };
  });

  indices.sort((a, b) => {
    for (const ctx of keyCtx) {
      const c = ctx.isString
        ? compareKey(ctx.bytes[a], ctx.bytes[b], ctx.key, compareBytes)
        : compareKey(ctx.values[a], ctx.values[b], ctx.key, compareScalar);
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

function compareScalar(a, b) {
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}

// One sort key, mirroring native LessByKey: nulls are placed by EffectiveNulls
// (default = Postgres: NULLS LAST for asc, NULLS FIRST for desc) independent of
// the value direction, which only flips the present-value comparison.
function compareKey(a, b, key, compare) {
  const aNull = a === null || a === undefined;
  const bNull = b === null || b === undefined;
  if (aNull || bNull) {
    if (aNull && bNull) {
      return 0;
    }
    const nullsFirst = key.nulls === 'nulls-first' ? true
      : key.nulls === 'nulls-last' ? false
      : key.direction === 'desc';
    return aNull === nullsFirst ? -1 : 1;
  }
  const c = compare(a, b);
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
