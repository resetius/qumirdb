// Browser execution runtime for simple one-to-one pipelines.
//
// Drives the standalone WASM kernels emitted by qdb_plan_export (bundle.exec):
// marshals a columnar batch into a kernel module's linear memory as
// TColumn[]/TRowSet per the verified 8-byte-pointer layout, runs the kernel, and
// reads results back. See PLAN_BROWSER_EXECUTION.md for the ABI/layout facts.

import { BucketAllocator } from './wasm_allocator.js';

const PAGE = 65536;
const EMPTY_BYTES = new Uint8Array(0);

function nowMs() {
  return globalThis.performance ? globalThis.performance.now() : Date.now();
}

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

function numericArrayConstructor(type) {
  switch (type) {
    case 'i8': return Int8Array;
    case 'u8': case 'bool': return Uint8Array;
    case 'i16': return Int16Array;
    case 'u16': return Uint16Array;
    case 'i32': return Int32Array;
    case 'u32': return Uint32Array;
    case 'i64': return BigInt64Array;
    case 'u64': return BigUint64Array;
    case 'f64': return Float64Array;
    default: return null;
  }
}

function typedColumnBytes(values, type, rowCount) {
  const Ctor = numericArrayConstructor(type);
  if (!Ctor || !(values instanceof Ctor) || values.length < rowCount) {
    return null;
  }
  const width = coreTypeWidth(type);
  return new Uint8Array(values.buffer, values.byteOffset, rowCount * width);
}

// Validity bitmap (LSB-first, 1 = valid) for a column whose values contain
// null; null when the column has no nulls (kernels treat a null mask pointer
// as all-valid, mirroring the native arrow source). Typed arrays cannot hold
// nulls.
function columnValidityMask(values, rowCount) {
  if (!values || ArrayBuffer.isView(values)) {
    return null;
  }
  let hasNull = false;
  for (let i = 0; i < rowCount; ++i) {
    const value = values[i];
    if (value === null || value === undefined) {
      hasNull = true;
      break;
    }
  }
  if (!hasNull) {
    return null;
  }
  const mask = new Uint8Array((rowCount + 7) >> 3);
  for (let i = 0; i < rowCount; ++i) {
    const value = values[i];
    if (value !== null && value !== undefined) {
      mask[i >> 3] |= 1 << (i & 7);
    }
  }
  return mask;
}

function makeRowId(batchIdx, rowIdx) {
  return (BigInt(batchIdx) << 32n) | BigInt(rowIdx >>> 0);
}

function encodeStringColumn(values, rowCount, encoder) {
  const parts = new Array(rowCount);
  const offsets = new Int32Array(rowCount + 1);
  const cache = new Map();
  let total = 0;
  for (let i = 0; i < rowCount; ++i) {
    const value = values[i];
    let b = EMPTY_BYTES;
    if (value !== null && value !== undefined) {
      const text = String(value);
      b = cache.get(text);
      if (!b) {
        b = encoder.encode(text);
        cache.set(text, b);
      }
    }
    parts[i] = b;
    offsets[i] = total;
    total += b.length;
  }
  offsets[rowCount] = total;
  return { parts, offsets, total };
}

class RowSetWriter {
  constructor(arena, layout) {
    this.arena = arena;
    this.layout = layout;
    this.columnsBase = 0;
    this.rowsetPtr = 0;
    this.dataPtrs = [];
    this.dataCaps = [];
    this.offsetsPtrs = [];
    this.offsetsCaps = [];
    this.selectionPtr = 0;
    this.selectionCap = 0;
  }

  write(columns, rowCount, wantSelection, selection = null) {
    this.ensureStructs(columns.length);

    const encoder = new TextEncoder();
    const encoded = columns.map(column => {
      if (column.type !== 'string') return null;
      return encodeStringColumn(column.values, rowCount, encoder);
    });
    const masks = columns.map(column => columnValidityMask(column.values, rowCount));

    for (let c = 0; c < columns.length; ++c) {
      const column = columns[c];
      if (column.type === 'string') {
        this.ensureData(c, Math.max(encoded[c].total, 1), 1);
        this.ensureOffsets(c, (rowCount + 1) * 4);
      } else {
        const width = coreTypeWidth(column.type);
        this.ensureData(c, Math.max(rowCount, 1) * width, 8);
      }
      if (masks[c]) {
        this.ensureMask(c, masks[c].length);
      }
    }
    if (wantSelection || selection) {
      this.ensureSelection(Math.max(rowCount, 1));
    }

    const colLayout = this.layout.column;
    const rsLayout = this.layout.rowset;
    const dv = this.arena.view();
    const memBytes = this.arena.bytes();

    for (let c = 0; c < columns.length; ++c) {
      const column = columns[c];
      const colPtr = this.columnsBase + c * colLayout.size;
      new Uint8Array(this.arena.memory.buffer, colPtr, colLayout.size).fill(0);
      writePointer(dv, colPtr + colLayout.data, this.dataPtrs[c]);
      if (masks[c]) {
        memBytes.set(masks[c], this.maskPtrs[c]);
        writePointer(dv, colPtr + colLayout.mask, this.maskPtrs[c]);
      }
      if (column.type === 'string') {
        const enc = encoded[c];
        let p = this.dataPtrs[c];
        for (let i = 0; i < rowCount; ++i) {
          memBytes.set(enc.parts[i], p);
          p += enc.parts[i].length;
        }
        for (let i = 0; i <= rowCount; ++i) {
          dv.setInt32(this.offsetsPtrs[c] + i * 4, enc.offsets[i], true);
        }
        writePointer(dv, colPtr + colLayout.offsets, this.offsetsPtrs[c]);
        dv.setUint8(colPtr + colLayout.offsetWidth, 4);
      } else {
        const width = coreTypeWidth(column.type);
        const values = column.values;
        const bytes = typedColumnBytes(values, column.type, rowCount);
        if (bytes) {
          memBytes.set(bytes, this.dataPtrs[c]);
        } else {
          for (let i = 0; i < rowCount; ++i) {
            writeNumericValue(dv, this.dataPtrs[c] + i * width, column.type, values[i]);
          }
        }
      }
    }

    new Uint8Array(this.arena.memory.buffer, this.rowsetPtr, rsLayout.size).fill(0);
    writePointer(dv, this.rowsetPtr + rsLayout.columns, this.columnsBase);
    dv.setBigInt64(this.rowsetPtr + rsLayout.columnCount, BigInt(columns.length), true);
    dv.setBigInt64(this.rowsetPtr + rsLayout.rowCount, BigInt(rowCount), true);
    if (selection) {
      copySelectionToMemory(this.arena, this.selectionPtr, selection, rowCount);
      writePointer(dv, this.rowsetPtr + rsLayout.selection, this.selectionPtr);
    } else if (wantSelection) {
      writePointer(dv, this.rowsetPtr + rsLayout.selection, this.selectionPtr);
    }

    return { rowsetPtr: this.rowsetPtr, selectionPtr: this.selectionPtr };
  }

  ensureStructs(columnCount) {
    if (this.rowsetPtr && this.dataPtrs.length === columnCount) {
      return;
    }
    const colLayout = this.layout.column;
    const rsLayout = this.layout.rowset;
    this.columnsBase = this.arena.alloc(columnCount * colLayout.size, 8);
    this.rowsetPtr = this.arena.alloc(rsLayout.size, 8);
    this.dataPtrs = new Array(columnCount).fill(0);
    this.dataCaps = new Array(columnCount).fill(0);
    this.offsetsPtrs = new Array(columnCount).fill(0);
    this.offsetsCaps = new Array(columnCount).fill(0);
    this.maskPtrs = new Array(columnCount).fill(0);
    this.maskCaps = new Array(columnCount).fill(0);
  }

  ensureMask(index, bytes) {
    if (this.maskCaps[index] >= bytes) {
      return;
    }
    this.maskPtrs[index] = this.arena.alloc(bytes, 8);
    this.maskCaps[index] = bytes;
  }

  ensureData(index, bytes, align) {
    if (this.dataCaps[index] >= bytes) {
      return;
    }
    this.dataPtrs[index] = this.arena.alloc(bytes, align);
    this.dataCaps[index] = bytes;
  }

  ensureOffsets(index, bytes) {
    if (this.offsetsCaps[index] >= bytes) {
      return;
    }
    this.offsetsPtrs[index] = this.arena.alloc(bytes, 4);
    this.offsetsCaps[index] = bytes;
  }

  ensureSelection(bytes) {
    if (this.selectionCap >= bytes) {
      return;
    }
    this.selectionPtr = this.arena.alloc(bytes, 8);
    this.selectionCap = bytes;
  }
}

class WasmRowStore {
  constructor(arena, layout) {
    this.arena = arena;
    this.layout = layout;
    this.batches = [];
    this.ownedPtrs = []; // per batch: our marshalled rowset ptr to free, or 0 (pinned)
    this.storePtr = 0;
    this.capacity = 0;
  }

  // `ownedPtr` is the marshalRowSet rowset this store may free at teardown; pass
  // 0 when `rowsetPtr` is a pinned buffer owned by an upstream operator.
  push(batch, rowsetPtr, ownedPtr = 0) {
    this.ensureCapacity(this.batches.length + 1);
    const idx = this.batches.length;
    this.batches.push(batch);
    this.ownedPtrs.push(ownedPtr);
    const size = this.layout.rowset.size;
    const memBytes = this.arena.bytes();
    memBytes.copyWithin(
      this.storePtr + idx * size,
      rowsetPtr,
      rowsetPtr + size);
    return idx;
  }

  // Frees the column buffers of every batch this store marshalled itself, plus
  // the store array. Build sides linger for the whole query otherwise — the
  // dominant cross-join accumulation in multi-join plans (TPC-H Q18/Q21).
  // Pinned batches (ownedPtr 0) are either source-owned or kernel-owned; the
  // latter carries a destroy hook on batch.wasm.
  freeMarshalled() {
    for (let i = 0; i < this.ownedPtrs.length; ++i) {
      const owned = this.ownedPtrs[i];
      if (!owned) {
        releaseWasmRowSet({ batch: this.batches[i] });
        continue;
      }
      freeMarshalledRowSet(this.arena, this.layout, owned);
    }
    if (this.storePtr) this.arena.free(this.storePtr);
    this.storePtr = 0;
    this.capacity = 0;
    this.batches = [];
    this.ownedPtrs = [];
  }

  dataPtr() {
    return this.storePtr;
  }

  batch(index) {
    return this.batches[index];
  }

  ensureCapacity(needed) {
    if (this.capacity >= needed) {
      return;
    }
    const size = this.layout.rowset.size;
    const next = Math.max(needed, this.capacity ? this.capacity * 2 : 4);
    const oldPtr = this.storePtr;
    const oldBytes = this.batches.length * size;
    this.storePtr = this.arena.alloc(next * size, 8);
    this.capacity = next;
    if (oldPtr && oldBytes > 0) {
      const memBytes = this.arena.bytes();
      memBytes.copyWithin(this.storePtr, oldPtr, oldPtr + oldBytes);
    }
  }
}

// One query-wide allocator over the shared linear memory. Backed by a
// segregated free-list (BucketAllocator) so qdb_free / qdb_realloc actually
// reclaim — bounding memory for join-heavy queries whose per-drain output and
// per-batch input buffers would otherwise accumulate forever in a bump arena.
// Blocks are >= 16-byte aligned, satisfying every caller's alignment request.
// As before, callers must finish all alloc() calls before taking a view()/
// bytes() (a grow inside malloc detaches the underlying ArrayBuffer).
class Arena {
  constructor(memory, base) {
    this.memory = memory;
    this.allocator = new BucketAllocator(memory, Number(base));
  }

  alloc(bytes, _align = 8) {
    return this.allocator.malloc(bytes);
  }

  realloc(ptr, bytes) {
    return this.allocator.realloc(ptr, bytes);
  }

  free(ptr) {
    this.allocator.free(ptr);
  }

  view() {
    return new DataView(this.memory.buffer);
  }

  bytes() {
    return new Uint8Array(this.memory.buffer);
  }
}

function createMemory64(initialPages) {
  const pages = BigInt(initialPages);
  const errors = [];
  for (const key of ['address', 'index']) {
    try {
      return new WebAssembly.Memory({ initial: pages, [key]: 'i64' });
    } catch (error) {
      errors.push(`${key}: ${error.message || String(error)}`);
    }
  }
  throw new Error(`memory64 is not available (${errors.join('; ')})`);
}

function createSharedMemory(layout) {
  const spec = layout.sharedMemory;
  if (!spec) {
    throw new Error('exec layout is missing sharedMemory');
  }
  const memory = createMemory64(Number(spec.initialPages));
  return {
    memory,
    arena: new Arena(memory, Number(spec.heapBase)),
  };
}

// batch.wasm is a pointer into the current query's shared memory. Streaming
// pointers are valid until the producing RowSetWriter writes again; pinned
// pointers are fresh arena allocations and can be retained by join state.
function selectionPtrOf(selection) {
  return selection?.kind === 'wasm-selection' ? selection.ptr : 0;
}

function attachBatchWasm(
    batch, rowsetPtr, { pinned, selectionPtr = 0, destroy = null }) {
  batch.wasm = {
    rowsetPtr,
    pinned: pinned === true,
    hasSelection: selectionPtr !== 0,
    selectionPtr,
    destroy,
  };
}

function wasmRowSetForSelection(batch, selection) {
  const wasm = batch?.wasm;
  if (!wasm) {
    return null;
  }
  if (selection == null) {
    return wasm.hasSelection ? null : wasm;
  }
  const selectionPtr = selectionPtrOf(selection);
  return selectionPtr !== 0 && wasm.hasSelection && wasm.selectionPtr === selectionPtr
    ? wasm
    : null;
}

function makeWasmSelection(arena, rowCount, ptr) {
  return ptr
    ? { kind: 'wasm-selection', arena, rowCount, ptr }
    : null;
}

function rowSelected(selection, row) {
  if (!selection) {
    return true;
  }
  if (selection.kind === 'wasm-selection') {
    return selection.arena.bytes()[selection.ptr + row] !== 0;
  }
  return selection[row] !== 0;
}

function copySelectionToMemory(arena, destPtr, selection, rowCount) {
  const out = arena.bytes();
  if (selection?.kind === 'wasm-selection') {
    out.set(
      selection.arena.bytes().subarray(selection.ptr, selection.ptr + rowCount),
      destPtr);
  } else {
    out.set(selection.subarray(0, rowCount), destPtr);
  }
}

function ensureRowSetSelection(arena, layout, rowsetPtr, rowCount) {
  const dv = arena.view();
  const selectionOffset = rowsetPtr + layout.rowset.selection;
  let selectionPtr = readPointer(dv, selectionOffset);
  if (!selectionPtr) {
    selectionPtr = arena.alloc(Math.max(rowCount, 1), 8);
    writePointer(arena.view(), selectionOffset, selectionPtr);
  }
  return selectionPtr;
}

function shapeColumns(output) {
  return (output || []).map(column => ({
    name: column.name,
    type: column.type,
  }));
}

function makeWasmOwnedBatch(state, rowsetPtr, output) {
  const rowCount = Number(
    state.arena.view().getBigInt64(rowsetPtr + state.layout.rowset.rowCount, true));
  const batch = {
    rowCount,
    columns: shapeColumns(output),
  };
  attachBatchWasm(batch, rowsetPtr, {
    pinned: true,
    destroy: () => destroyKernelOwnedRowSet(state, rowsetPtr),
  });
  return batch;
}

function pinSourceBatch(batch, arena, layout) {
  if (batch.wasm) {
    return batch;
  }
  const marshalled = marshalRowSet(
    arena, layout, batch.columns, batch.rowCount, false, null);
  attachBatchWasm(batch, marshalled.rowsetPtr, {
    pinned: true,
    destroy: () => freeMarshalledRowSet(arena, layout, marshalled.rowsetPtr),
  });
  return batch;
}

function takeRowSetRelease(rowSet) {
  const releases = [];
  if (typeof rowSet?.destroy === 'function') {
    releases.push(rowSet.destroy);
    rowSet.destroy = null;
  }
  const wasm = rowSet?.batch?.wasm;
  if (typeof wasm?.destroy === 'function') {
    releases.push(wasm.destroy);
    wasm.destroy = null;
  }
  if (releases.length === 0) {
    return null;
  }
  return () => {
    for (const release of releases) {
      release();
    }
  };
}

function releaseWasmRowSet(rowSet) {
  if (typeof rowSet?.destroy === 'function') {
    const destroy = rowSet.destroy;
    rowSet.destroy = null;
    destroy();
  }
  const wasm = rowSet?.batch?.wasm;
  if (typeof wasm?.destroy === 'function') {
    const destroy = wasm.destroy;
    wasm.destroy = null;
    destroy();
  }
}

function freeMarshalledRowSet(arena, layout, rowsetPtr) {
  if (!rowsetPtr) {
    return;
  }
  const dv = arena.view();
  const col = layout.column;
  const rs = layout.rowset;
  const colsBase = readPointer(dv, rowsetPtr + rs.columns);
  const colCount = Number(dv.getBigInt64(rowsetPtr + rs.columnCount, true));
  if (colsBase) {
    for (let c = 0; c < colCount; ++c) {
      const colPtr = colsBase + c * col.size;
      arena.free(readPointer(dv, colPtr + col.data));
      arena.free(readPointer(dv, colPtr + col.mask));
      arena.free(readPointer(dv, colPtr + col.offsets));
    }
    arena.free(colsBase);
  }
  arena.free(readPointer(dv, rowsetPtr + rs.selection));
  arena.free(rowsetPtr);
}

function writePointer(dv, offset, address) {
  // Pointers are 8-byte fields; the address lives in the low 32 bits.
  dv.setUint32(offset, address >>> 0, true);
  dv.setUint32(offset + 4, 0, true);
}

function readPointer(dv, offset) {
  return Number(dv.getBigUint64(offset, true));
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

function divTrunc(n, d) {
  return Math.trunc(n / d);
}

function daysFromCivil(y, m, d) {
  y -= m <= 2 ? 1 : 0;
  const era = divTrunc(y >= 0 ? y : y - 399, 400);
  const yoe = y - era * 400;
  const doy = divTrunc(153 * (m + (m > 2 ? -3 : 9)) + 2, 5) + d - 1;
  const doe = yoe * 365 + divTrunc(yoe, 4) - divTrunc(yoe, 100) + doy;
  return (era * 146097 + doe - 719468) | 0;
}

function dateYear(days) {
  const z = (Number(days) | 0) + 719468;
  const era = divTrunc(z >= 0 ? z : z - 146096, 146097);
  const doe = z - era * 146097;
  const yoe = divTrunc(doe - divTrunc(doe, 1460) + divTrunc(doe, 36524) - divTrunc(doe, 146096), 365);
  let y = yoe + era * 400;
  const doy = doe - (365 * yoe + divTrunc(yoe, 4) - divTrunc(yoe, 100));
  const mp = divTrunc(5 * doy + 2, 153);
  y += mp >= 10 ? 1 : 0;
  return y | 0;
}

// The `env` imports the kernels need. Pointer + size args are i64 (BigInt
// under memory64); bool results are i32 (0/1), compare/like are i64.
// `getMemory` is read lazily since the instance doesn't exist yet at build
// time; `holder.arena` is bound by drivers whose kernels allocate
// (qdb_alloc/realloc/free — e.g. the aggregate hash table).
function createQdbEnv(getMemory, holder) {
  const alloc = (size) => {
    if (!holder.arena) {
      throw new Error('kernel called qdb_alloc but no allocator is bound');
    }
    return holder.arena.alloc(Math.max(Number(size), 1), 8);
  };
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
  const cstrEquals = (ptr, text) => {
    if (Number(ptr) === 0) return false;
    const bytes = cstrAt(ptr);
    if (bytes.length !== text.length) return false;
    for (let i = 0; i < bytes.length; ++i) {
      if (bytes[i] !== text.charCodeAt(i)) return false;
    }
    return true;
  };
  const atoi = (ptr) => {
    if (Number(ptr) === 0) return 0;
    const bytes = cstrAt(ptr);
    let i = 0;
    while (i < bytes.length && (bytes[i] === 32 || (bytes[i] >= 9 && bytes[i] <= 13))) ++i;
    let sign = 1;
    if (bytes[i] === 45 || bytes[i] === 43) {
      sign = bytes[i] === 45 ? -1 : 1;
      ++i;
    }
    let value = 0;
    while (i < bytes.length && bytes[i] >= 48 && bytes[i] <= 57) {
      value = value * 10 + (bytes[i] - 48);
      ++i;
    }
    return (sign * value) | 0;
  };

  const svLit = (t) => (d, s, lit) => (t(compareBytes(bytesAt(d, s), cstrAt(lit))) ? 1 : 0);
  const litSv = (t) => (lit, d, s) => (t(compareBytes(cstrAt(lit), bytesAt(d, s))) ? 1 : 0);
  const litLit = (t) => (l, r) => (t(compareBytes(cstrAt(l), cstrAt(r))) ? 1 : 0);
  const EQ = c => c === 0, NE = c => c !== 0;
  const LT = c => c < 0, LE = c => c <= 0, GT = c => c > 0, GE = c => c >= 0;

  const sqlDate = (date) => {
    if (Number(date) === 0) return 0;
    const bytes = cstrAt(date);
    const parts = [0, 0, 0];
    let idx = 0;
    for (const b of bytes) {
      if (idx >= 3) break;
      if (b === 45) {
        ++idx;
      } else if (b >= 48 && b <= 57) {
        parts[idx] = parts[idx] * 10 + (b - 48);
      }
    }
    return daysFromCivil(parts[0], parts[1], parts[2]);
  };
  const sqlInterval = (amount, unit) => {
    const n = atoi(amount);
    if (cstrEquals(unit, 'year') || cstrEquals(unit, 'years')) return (n * 365) | 0;
    if (cstrEquals(unit, 'month') || cstrEquals(unit, 'months')) return (n * 30) | 0;
    return n;
  };
  const substring = (out, data, size, start, length) => {
    const strSize = Number(size);
    let offset = Number(start) - 1;
    if (offset < 0) offset = 0;
    let len = Number(length);
    if (offset >= strSize) {
      offset = strSize;
      len = 0;
    } else {
      if (len < 0) len = 0;
      if (offset + len > strSize) len = strSize - offset;
    }
    const dv = new DataView(getMemory().buffer);
    dv.setBigUint64(Number(out), BigInt(data) + BigInt(offset), true);
    dv.setBigInt64(Number(out) + 8, BigInt(len), true);
  };

  const norm = v => (v < 0n ? 1n : v);
  return {
    // malloc family over the query's shared linear memory (segregated
    // free-list; realloc/free reclaim).
    qdb_alloc: (size) => BigInt(alloc(size)),
    qdb_realloc: (ptr, size) =>
      BigInt(holder.arena.realloc(Number(ptr), Math.max(Number(size), 1))),
    qdb_free: (ptr) => holder.arena.free(Number(ptr)),

    qdb_filter_string_compare: (ld, ls, rd, rs) =>
      BigInt(compareBytes(bytesAt(ld, ls), bytesAt(rd, rs))),
    qdb_string_view_cmp_cstr: (d, s, c) => BigInt(compareBytes(bytesAt(d, s), cstrAt(c))),
    qdb_cstr_cmp_cstr: (a, b) => BigInt(compareBytes(cstrAt(a), cstrAt(b))),
    qdb_string_view_sql_like: (d, s, p) => BigInt(sqlLikeBytes(bytesAt(d, s), cstrAt(p))),
    qdb_substring: substring,
    qdb_date_year: dateYear,
    qdb_sql_date: sqlDate,
    qdb_sql_interval: sqlInterval,
    qdb_bitmap_set_valid: (bitmap, index, valid) => {
      const mem = new Uint8Array(getMemory().buffer);
      const i = BigInt(index);
      const byteIndex = Number(bitmap) + Number(i >> 3n);
      const bit = 1 << Number(i & 7n);
      if (valid) mem[byteIndex] |= bit;
      else mem[byteIndex] &= ~bit;
    },

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

// Compile + instantiate a kernel module, wiring the `env` runtime. Any import
// we do not implement is reported immediately instead of running a partial link.
export async function instantiateKernel(base64, entryName, shared) {
  const module = await WebAssembly.compile(base64ToBytes(base64));
  const holder = { memory: shared.memory, arena: shared.arena };
  const env = createQdbEnv(() => holder.memory, holder);
  env.memory = shared.memory;
  for (const imp of WebAssembly.Module.imports(module)) {
    if (imp.module !== 'env') {
      throw new Error(`kernel needs unsupported import: ${imp.module}.${imp.name}`);
    }
    if (imp.name === 'memory' && imp.kind === 'memory') {
      continue;
    }
    if (typeof env[imp.name] !== 'function') {
      throw new Error(`kernel needs unsupported import: ${imp.module}.${imp.name}`);
    }
  }
  const instance = await WebAssembly.instantiate(module, { env });
  if (instance.exports.__wasm_call_ctors) {
    instance.exports.__wasm_call_ctors();
  }
  const fn = instance.exports[entryName];
  if (typeof fn !== 'function') {
    throw new Error(`kernel is missing entry ${entryName}`);
  }
  return { instance, fn, holder };
}

// Marshal `columns` (source column order) into a fresh, pinned TRowSet inside `arena`.
// Numeric columns get a data buffer; string columns get a concatenated UTF-8
// bytes buffer plus an i32 offsets array (OffsetWidth = 4), matching the layout
// the kernel reads (Data + Offsets[row]).
function marshalRowSet(arena, layout, columns, rowCount, wantSelection, selection = null) {
  const colLayout = layout.column;
  const rsLayout = layout.rowset;
  const encoder = new TextEncoder();

  // Pre-encode string columns before any allocation (a mid-write grow would
  // detach views): parts hold each row's bytes, offsets are cumulative.
  const encoded = columns.map(column => {
    if (column.type !== 'string') return null;
    return encodeStringColumn(column.values, rowCount, encoder);
  });

  const columnsBase = arena.alloc(columns.length * colLayout.size, 8);
  const dataPtrs = new Array(columns.length).fill(0);
  const offsetsPtrs = new Array(columns.length).fill(0);
  const masks = columns.map(column => columnValidityMask(column.values, rowCount));
  const maskPtrs = new Array(columns.length).fill(0);

  for (let c = 0; c < columns.length; ++c) {
    const column = columns[c];
    if (column.type === 'string') {
      dataPtrs[c] = arena.alloc(Math.max(encoded[c].total, 1), 1);
      offsetsPtrs[c] = arena.alloc((rowCount + 1) * 4, 4);
    } else {
      const width = coreTypeWidth(column.type);
      dataPtrs[c] = arena.alloc(Math.max(rowCount, 1) * width, 8);
    }
    if (masks[c]) {
      maskPtrs[c] = arena.alloc(masks[c].length, 8);
    }
  }

  const selectionPtr = (wantSelection || selection)
    ? arena.alloc(Math.max(rowCount, 1), 8)
    : 0;
  const rowsetPtr = arena.alloc(rsLayout.size, 8);

  // All allocations done; safe to take views and write.
  const dv = arena.view();
  const memBytes = arena.bytes();

  for (let c = 0; c < columns.length; ++c) {
    const column = columns[c];
    const colPtr = columnsBase + c * colLayout.size;
    new Uint8Array(arena.memory.buffer, colPtr, colLayout.size).fill(0);
    writePointer(dv, colPtr + colLayout.data, dataPtrs[c]);
    if (masks[c]) {
      memBytes.set(masks[c], maskPtrs[c]);
      writePointer(dv, colPtr + colLayout.mask, maskPtrs[c]);
    }
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
      const bytes = typedColumnBytes(values, column.type, rowCount);
      if (bytes) {
        memBytes.set(bytes, dataPtrs[c]);
      } else {
        for (let i = 0; i < rowCount; ++i) {
          writeNumericValue(dv, dataPtrs[c] + i * width, column.type, values[i]);
        }
      }
    }
  }

  new Uint8Array(arena.memory.buffer, rowsetPtr, rsLayout.size).fill(0);
  writePointer(dv, rowsetPtr + rsLayout.columns, columnsBase);
  dv.setBigInt64(rowsetPtr + rsLayout.columnCount, BigInt(columns.length), true);
  dv.setBigInt64(rowsetPtr + rsLayout.rowCount, BigInt(rowCount), true);
  if (selection) {
    copySelectionToMemory(arena, selectionPtr, selection, rowCount);
    writePointer(dv, rowsetPtr + rsLayout.selection, selectionPtr);
  } else if (wantSelection) {
    writePointer(dv, rowsetPtr + rsLayout.selection, selectionPtr);
  }

  return { rowsetPtr, selectionPtr, columnsBase };
}

function reusableFilterWasm(rowSet, pinned) {
  const wasm = wasmRowSetForSelection(rowSet.batch, rowSet.selection);
  return wasm && (wasm.pinned || !pinned) ? wasm : null;
}

// Run a filter kernel over `rowSet` (columns in source order). The kernel writes
// the result selection into the rowset's Selection buffer; JS carries only the
// pointer descriptor.
export function runFilter(kernel, layout, rowSet, writer, options = {}) {
  const pinned = options.pinned === true;
  const { batch, selection } = rowSet;
  const arena = kernel.holder.arena;
  const wasm = reusableFilterWasm(rowSet, pinned);
  let rowsetPtr;
  let selectionPtr;
  let destroy = null;
  if (wasm) {
    rowsetPtr = wasm.rowsetPtr;
    selectionPtr = ensureRowSetSelection(arena, layout, rowsetPtr, batch.rowCount);
    destroy = wasm.destroy || null;
  } else if (pinned) {
    const marshalled = marshalRowSet(
      arena, layout, batch.columns, batch.rowCount, true, selection);
    rowsetPtr = marshalled.rowsetPtr;
    selectionPtr = marshalled.selectionPtr;
    destroy = () => freeMarshalledRowSet(arena, layout, rowsetPtr);
  } else {
    const written = writer.write(
      batch.columns, batch.rowCount, true, selection);
    rowsetPtr = written.rowsetPtr;
    selectionPtr = written.selectionPtr;
  }
  // memory64 entry: the rowset pointer is an i64 param.
  kernel.fn(BigInt(rowsetPtr));
  attachBatchWasm(batch, rowsetPtr, {
    pinned: wasm ? wasm.pinned : pinned,
    selectionPtr,
    destroy,
  });
  return makeWasmSelection(arena, batch.rowCount, selectionPtr);
}

function stableProjectInput(rowSet, arena, layout) {
  const wasm = wasmRowSetForSelection(rowSet.batch, rowSet.selection);
  if (wasm?.pinned) {
    return {
      rowsetPtr: wasm.rowsetPtr,
      release: takeRowSetRelease(rowSet),
    };
  }
  const marshalled = marshalRowSet(
    arena,
    layout,
    rowSet.batch.columns,
    rowSet.batch.rowCount,
    false,
    rowSet.selection);
  const releaseInput = takeRowSetRelease(rowSet);
  return {
    rowsetPtr: marshalled.rowsetPtr,
    release: () => {
      freeMarshalledRowSet(arena, layout, marshalled.rowsetPtr);
      if (releaseInput) {
        releaseInput();
      }
    },
  };
}

function writeEmptyColumn(dv, colPtr, layout) {
  new Uint8Array(dv.buffer, colPtr, layout.column.size).fill(0);
}

function buildProjectStringColumn(arena, layout, stringViewPtr, rowCount) {
  const sv = layout.stringView;
  let dv = arena.view();
  const views = new Array(rowCount);
  let total = 0;
  for (let r = 0; r < rowCount; ++r) {
    const base = stringViewPtr + r * sv.size;
    const dataPtr = readPointer(dv, base + sv.data);
    const size = Number(dv.getBigInt64(base + sv.length, true));
    views[r] = { dataPtr, size, offset: total };
    total += size;
  }

  const dataPtr = arena.alloc(Math.max(total, 1), 1);
  const offsetsPtr = arena.alloc((rowCount + 1) * 8, 8);
  dv = arena.view();
  const memBytes = arena.bytes();
  let writeAt = dataPtr;
  for (let r = 0; r < rowCount; ++r) {
    const view = views[r];
    dv.setBigInt64(offsetsPtr + r * 8, BigInt(view.offset), true);
    if (view.size > 0 && view.dataPtr) {
      memBytes.set(
        memBytes.subarray(view.dataPtr, view.dataPtr + view.size),
        writeAt);
      writeAt += view.size;
    }
  }
  dv.setBigInt64(offsetsPtr + rowCount * 8, BigInt(total), true);
  return { dataPtr, offsetsPtr, offsetWidth: 8 };
}

// Run a project kernel over `rowSet` and build an output TRowSet in wasm memory.
// Passthrough columns copy only TColumn descriptors; computed columns own their
// result buffers. The output destroy hook releases the retained input rowset.
export function runProject(kernel, layout, rowSet, output, arena) {
  arena = arena || kernel?.holder?.arena || rowSet.selection?.arena;
  if (!arena) {
    throw new Error('project needs shared wasm memory');
  }
  const rowCount = rowSet.batch.rowCount;
  const computed = output.filter(col => col.source === 'computed');
  if (computed.length > 0 && !kernel) {
    throw new Error('project stage is missing wasm kernel');
  }

  const input = stableProjectInput(rowSet, arena, layout);
  const ownedPtrs = [];
  let inputRelease = input.release;
  let rowsetPtr = 0;
  let columnsBase = 0;
  let outArrayPtr = 0;
  const computedBuffers = [];

  try {
    if (computed.length > 0) {
      outArrayPtr = arena.alloc(computed.length * 8, 8);
      for (let k = 0; k < computed.length; ++k) {
        const width = Number(computed[k].width || 0);
        if (width <= 0) {
          throw new Error(`unsupported project column width: ${computed[k].name}`);
        }
        const ptr = arena.alloc(Math.max(rowCount, 1) * width, 8);
        computedBuffers.push(ptr);
      }
      let dv = arena.view();
      for (let k = 0; k < computedBuffers.length; ++k) {
        writePointer(dv, outArrayPtr + k * 8, computedBuffers[k]);
      }
      kernel.fn(BigInt(input.rowsetPtr), BigInt(outArrayPtr));
      arena.free(outArrayPtr);
      outArrayPtr = 0;
    }

    const computedColumns = computedBuffers.map((ptr, k) => {
      const spec = computed[k];
      if (!spec.isString) {
        ownedPtrs.push(ptr);
        return { dataPtr: ptr, offsetsPtr: 0, offsetWidth: 0 };
      }
      const stringColumn = buildProjectStringColumn(arena, layout, ptr, rowCount);
      arena.free(ptr);
      ownedPtrs.push(stringColumn.dataPtr, stringColumn.offsetsPtr);
      return stringColumn;
    });

    rowsetPtr = arena.alloc(layout.rowset.size, 8);
    columnsBase = arena.alloc(output.length * layout.column.size, 8);
    ownedPtrs.push(columnsBase, rowsetPtr);

    let dv = arena.view();
    const memBytes = arena.bytes();
    const inputColumns = readPointer(dv, input.rowsetPtr + layout.rowset.columns);
    const selectionPtr = readPointer(dv, input.rowsetPtr + layout.rowset.selection);
    let computedCursor = 0;
    for (let i = 0; i < output.length; ++i) {
      const spec = output[i];
      const outCol = columnsBase + i * layout.column.size;
      if (spec.source === 'passthrough') {
        const inCol = inputColumns + Number(spec.inputIndex) * layout.column.size;
        memBytes.copyWithin(outCol, inCol, inCol + layout.column.size);
        continue;
      }
      writeEmptyColumn(dv, outCol, layout);
      const computedColumn = computedColumns[computedCursor++];
      writePointer(dv, outCol + layout.column.data, computedColumn.dataPtr);
      if (computedColumn.offsetsPtr) {
        writePointer(dv, outCol + layout.column.offsets, computedColumn.offsetsPtr);
        dv.setUint8(outCol + layout.column.offsetWidth, computedColumn.offsetWidth);
      }
    }

    new Uint8Array(arena.memory.buffer, rowsetPtr, layout.rowset.size).fill(0);
    dv = arena.view();
    writePointer(dv, rowsetPtr + layout.rowset.columns, columnsBase);
    dv.setBigInt64(rowsetPtr + layout.rowset.columnCount, BigInt(output.length), true);
    dv.setBigInt64(rowsetPtr + layout.rowset.rowCount, BigInt(rowCount), true);
    if (selectionPtr) {
      writePointer(dv, rowsetPtr + layout.rowset.selection, selectionPtr);
    }

    const batch = {
      rowCount,
      columns: shapeColumns(output),
    };
    const destroy = () => {
      for (const ptr of ownedPtrs) {
        arena.free(ptr);
      }
      if (inputRelease) {
        inputRelease();
        inputRelease = null;
      }
    };
    attachBatchWasm(batch, rowsetPtr, {
      pinned: true,
      selectionPtr,
      destroy,
    });
    return {
      batch,
      selection: makeWasmSelection(arena, rowCount, selectionPtr),
    };
  } catch (error) {
    if (outArrayPtr) {
      arena.free(outArrayPtr);
    }
    for (const ptr of computedBuffers) {
      arena.free(ptr);
    }
    for (const ptr of ownedPtrs) {
      arena.free(ptr);
    }
    if (inputRelease) {
      inputRelease();
    }
    throw error;
  }
}

// Read `count` validity bits (LSB-first per byte, qdb_bitmap_set_valid order).
function readValidityBits(memBytes, maskPtr, count) {
  const valid = new Array(count);
  for (let i = 0; i < count; ++i) {
    valid[i] = (memBytes[maskPtr + (i >> 3)] & (1 << (i & 7))) !== 0;
  }
  return valid;
}

function materializeWasmRowSet(arena, layout, output, rowsetPtr) {
  const dv = arena.view();
  const memBytes = arena.bytes();
  const rowCount = Number(dv.getBigInt64(rowsetPtr + layout.rowset.rowCount, true));
  const columnsPtr = readPointer(dv, rowsetPtr + layout.rowset.columns);
  const decoder = new TextDecoder();
  const columns = [];

  for (let i = 0; i < output.length; ++i) {
    const spec = output[i];
    const colPtr = columnsPtr + i * layout.column.size;
    const dataPtr = readPointer(dv, colPtr + layout.column.data);
    const maskPtr = readPointer(dv, colPtr + layout.column.mask);
    const offsetsPtr = readPointer(dv, colPtr + layout.column.offsets);
    const offsetWidth = dv.getUint8(colPtr + layout.column.offsetWidth);
    const valid = maskPtr ? readValidityBits(memBytes, maskPtr, rowCount) : null;
    const values = new Array(rowCount);

    if (spec.type === 'string') {
      for (let r = 0; r < rowCount; ++r) {
        if (valid && !valid[r]) {
          values[r] = null;
          continue;
        }
        const begin = offsetWidth === 4
          ? dv.getInt32(offsetsPtr + r * 4, true)
          : Number(dv.getBigInt64(offsetsPtr + r * 8, true));
        const end = offsetWidth === 4
          ? dv.getInt32(offsetsPtr + (r + 1) * 4, true)
          : Number(dv.getBigInt64(offsetsPtr + (r + 1) * 8, true));
        values[r] = decoder.decode(memBytes.subarray(dataPtr + begin, dataPtr + end));
      }
    } else {
      const width = coreTypeWidth(spec.type);
      for (let r = 0; r < rowCount; ++r) {
        values[r] = (valid && !valid[r])
          ? null
          : readNumericValue(dv, dataPtr + r * width, spec.type);
      }
    }
    columns.push({ name: spec.name, type: spec.type, values });
  }

  const batch = { rowCount, columns };
  attachBatchWasm(batch, rowsetPtr, { pinned: true });
  return batch;
}

// Frees a kernel-materialized rowset: walks the owners list stored in Private
// (owners[0] = count, then one qdb_alloc'd pointer per buffer), frees each
// buffer, then the owners array and the rowset shell. Values must already be
// copied to JS (materializeWasmRowSet) — the wasm memory is dead afterward.
function destroyKernelOwnedRowSet(state, rowsetPtr) {
  const free = (p) => state.arena.free(p);
  const dv = state.arena.view();
  const ownersPtr = readPointer(dv, rowsetPtr + state.layout.rowset.private);
  if (ownersPtr) {
    const count = Number(dv.getBigInt64(ownersPtr, true));
    for (let i = 0; i < count; ++i) {
      free(readPointer(dv, ownersPtr + (i + 1) * 8));
    }
    free(ownersPtr);
  }
  free(rowsetPtr);
}

// Materializes one output batch from a join/cross pair buffer. When downstream
// can consume wasm rowsets, keep the kernel-owned TRowSet alive and return only
// a handle. Compatibility callers still get JS arrays and immediately free the
// wasm buffers.
function drainMaterializedBatch(
    state, maxRows, streamLeftPtr, streamRightPtr, asWasm = false) {
  const { layout, arena } = state;
  const rowsetPtr = arena.alloc(layout.rowset.size, 8);
  const leftPtr = BigInt(state.leftStore.dataPtr());
  const rightPtr = BigInt(state.rightStore.dataPtr());
  // For streamed pairs (batch index -1 on a side) jt_materialize reads that side
  // from the stream_* rowset rather than the store; buffered pairs ignore them.
  const streamLeft = streamLeftPtr !== undefined ? BigInt(streamLeftPtr) : leftPtr;
  const streamRight = streamRightPtr !== undefined ? BigInt(streamRightPtr) : rightPtr;
  const produced = Number(state.materialize(
    BigInt(state.pairBuffer),
    leftPtr, rightPtr, streamLeft, streamRight,
    BigInt(state.materializeCursor),
    BigInt(maxRows),
    BigInt(rowsetPtr)));
  if (produced <= 0) {
    arena.free(rowsetPtr);
    throw new Error('join materialize failed');
  }
  state.materializeCursor += produced;
  if (asWasm) {
    return makeWasmOwnedBatch(state, rowsetPtr, state.stage.output || []);
  }
  const batch = materializeWasmRowSet(
    arena, layout, state.stage.output || [], rowsetPtr);
  destroyKernelOwnedRowSet(state, rowsetPtr);
  delete batch.wasm; // buffers are freed; force a re-marshal downstream
  return batch;
}

// Drive the fused aggregate kernel. agg_dispatch updates the hash table;
// agg_finish_rowset owns measure/finalize/output buffer allocation and writes a
// complete TRowSet in linear memory.
export function runAggregate(kernel, layout, batch, selection, stage) {
  const state = createAggregateState(kernel, layout, stage);
  updateAggregateState(state, { batch, selection });
  return finishAggregateState(state);
}

function createAggregateState(kernel, layout, stage) {
  const dispatch = kernel.instance.exports['agg_dispatch'];
  const finishRowSet = kernel.instance.exports['agg_finish_rowset'];
  if (!dispatch || !finishRowSet) {
    throw new Error('aggregate kernel is missing an entry');
  }

  const arena = kernel.holder.arena; // qdb_alloc draws from the shared bump pointer

  const output = stage.output;

  // init(ht, capacity)
  const ht = arena.alloc(layout.hashTable.size, 8);
  new Uint8Array(arena.memory.buffer, ht, layout.hashTable.size).fill(0);
  dispatch(BigInt(ht), 0n, 4n, 0n);

  return {
    kernel,
    layout,
    stage,
    dispatch,
    finishRowSet,
    arena,
    ht,
    output,
    inputWriter: new RowSetWriter(arena, layout),
    finished: false,
  };
}

function updateAggregateState(state, rowSet) {
  if (state.finished) {
    throw new Error('aggregate state is already finalized');
  }
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  // update(ht, batch) — the kernel honors rowset.Selection.
  const rowsetPtr = wasm
    ? wasm.rowsetPtr
    : state.inputWriter.write(
        batch.columns, batch.rowCount, false, selection).rowsetPtr;
  state.dispatch(BigInt(state.ht), BigInt(rowsetPtr), 0n, 1n);
}

function finishAggregateState(state, asWasm = false) {
  if (state.finished) {
    throw new Error('aggregate state is already finalized');
  }
  state.finished = true;
  const {
    layout,
    finishRowSet,
    arena,
    ht,
    output,
  } = state;

  const expectedSize = Number(
    arena.view().getBigInt64(ht + layout.hashTable.sizeOffset, true));
  const rowsetPtr = arena.alloc(layout.rowset.size, 8);
  new Uint8Array(arena.memory.buffer, rowsetPtr, layout.rowset.size).fill(0);
  const finalized = Number(finishRowSet(BigInt(ht), BigInt(rowsetPtr)));
  if (finalized !== expectedSize) {
    throw new Error('aggregate finalize returned an unexpected row count');
  }
  if (asWasm) {
    return makeWasmOwnedBatch(state, rowsetPtr, output);
  }
  const batch = materializeWasmRowSet(arena, layout, output, rowsetPtr);
  destroyKernelOwnedRowSet(state, rowsetPtr);
  delete batch.wasm;
  return batch;
}

const JoinOp = Object.freeze({
  INIT: 0n,
  UPDATE_LEFT: 1n,
  UPDATE_RIGHT: 2n,
  STREAM_LEFT: 3n,
  STREAM_RIGHT: 4n,
  FINALIZE: 5n,
  DESTROY: 6n,
});

const CrossJoinOp = Object.freeze({
  EMIT: 0n,
  DESTROY: 1n,
});

function createJoinState(kernel, layout, stage) {
  if (!browserRuntimeSupportsJoin(stage)) {
    throw new Error(`browser join type is not implemented: ${stage.joinType}` +
      (stage.hasResidual ? ' with residual' : ''));
  }
  const exports = kernel.instance.exports;
  const dispatch = exports.jt_dispatch;
  const materialize = exports.jt_materialize;
  if (typeof dispatch !== 'function' || typeof materialize !== 'function') {
    throw new Error('join kernel is missing an entry');
  }

  const arena = kernel.holder.arena;
  const leftTable = arena.alloc(layout.hashTable.size, 8);
  const rightTable = arena.alloc(layout.hashTable.size, 8);
  const pairBuffer = arena.alloc(layout.pairBuffer.size, 8);
  new Uint8Array(arena.memory.buffer, leftTable, layout.hashTable.size).fill(0);
  new Uint8Array(arena.memory.buffer, rightTable, layout.hashTable.size).fill(0);
  new Uint8Array(arena.memory.buffer, pairBuffer, layout.pairBuffer.size).fill(0);
  if (!dispatch(
      BigInt(leftTable),
      BigInt(rightTable),
      0n,
      0n,
      BigInt(pairBuffer),
      0n,
      0n,
      256n,
      JoinOp.INIT)) {
    throw new Error('join hash table initialization failed');
  }

  return {
    kernel,
    layout,
    stage,
    dispatch,
    materialize,
    arena,
    leftTable,
    rightTable,
    pairBuffer,
    leftStore: new WasmRowStore(arena, layout),
    rightStore: new WasmRowStore(arena, layout),
    // Reused marshalling buffer for the streamed (probe) side of an inner join:
    // the right side is probed batch-by-batch and never stored, so a grow-only
    // writer keeps its memory bounded (mirrors the filter/project/aggregate
    // input path).
    streamWriter: new RowSetWriter(arena, layout),
    materializeCursor: 0,
    semiAntiFinalized: false,
    outerFinalized: false,
    finalized: false,
  };
}

function isInnerJoin(stage) {
  return stage.joinType === 'inner';
}

// Streams one probe-side (right) batch of an INNER join: builds the left table
// beforehand, so the right side only probes (STREAM_RIGHT = jt_probe_right_stream,
// no insert) and is never stored. Pairs carry right batch index -1 and are
// materialized immediately against the live batch, then the pair buffer is
// reset. Bounds memory to the build (left) side alone. Returns the output batches.
function streamJoinRightBatch(state, rowSet, asWasm = false) {
  const { arena, layout } = state;
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  const rowsetPtr = wasm
    ? wasm.rowsetPtr
    : state.streamWriter.write(
        batch.columns, batch.rowCount, false, selection).rowsetPtr;
  try {
    const ok = state.dispatch(
      BigInt(state.leftTable),
      BigInt(state.rightTable),
      BigInt(rowsetPtr),
      -1n,
      BigInt(state.pairBuffer),
      BigInt(state.leftStore.dataPtr()),
      BigInt(state.rightStore.dataPtr()),
      0n,
      JoinOp.STREAM_RIGHT);
    if (!ok) {
      throw new Error('join stream failed');
    }
    const pairLayout = layout.pairBuffer;
    const count = Number(
      arena.view().getBigInt64(state.pairBuffer + pairLayout.count, true));
    const results = [];
    while (state.materializeCursor < count) {
      // stream_right = the live right rowset; stream_left unused (left ids are stored).
      results.push(drainMaterializedBatch(
        state, 1024, state.leftStore.dataPtr(), rowsetPtr, asWasm));
    }
    arena.view().setBigInt64(state.pairBuffer + pairLayout.count, 0n, true);
    state.materializeCursor = 0;
    return results;
  } finally {
    releaseWasmRowSet(rowSet);
  }
}

function browserRuntimeSupportsJoin(stage) {
  if (stage.joinType === 'inner') return true;
  if (isOuterJoin(stage) && !stage.hasResidual) return true;
  return isLeftSemiAntiJoin(stage);
}

function updateJoinState(state, side, rowSet) {
  const { arena, layout } = state;
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  const marshalled = wasm?.pinned
    ? null
    : marshalRowSet(
        arena, layout, batch.columns, batch.rowCount, false, selection);
  const rowsetPtr = wasm?.pinned ? wasm.rowsetPtr : marshalled.rowsetPtr;
  const isLeft = side === 0;
  const semiAnti = isLeftSemiAntiJoin(state.stage);
  let batchIdx = -1;
  let batchPtr = rowsetPtr;
  let stored = false;
  if (isLeft || !semiAnti) {
    const store = isLeft ? state.leftStore : state.rightStore;
    batchIdx = store.push(batch, rowsetPtr, wasm?.pinned ? 0 : rowsetPtr);
    batchPtr = store.dataPtr() + batchIdx * layout.rowset.size;
    stored = true;
  }
  try {
    const ok = isLeft
      ? state.dispatch(
          BigInt(state.leftTable),
          BigInt(state.rightTable),
          BigInt(batchPtr),
          BigInt(batchIdx),
          BigInt(state.pairBuffer),
          BigInt(state.leftStore.dataPtr()),
          BigInt(state.rightStore.dataPtr()),
          0n,
          JoinOp.UPDATE_LEFT)
      : state.dispatch(
          BigInt(state.leftTable),
          BigInt(state.rightTable),
          BigInt(batchPtr),
          BigInt(batchIdx),
          BigInt(state.pairBuffer),
          BigInt(state.leftStore.dataPtr()),
          BigInt(state.rightStore.dataPtr()),
          0n,
          JoinOp.UPDATE_RIGHT);
    if (!ok) {
      throw new Error('join kernel update failed');
    }
  } finally {
    if (marshalled) {
      releaseWasmRowSet(rowSet);
    }
    if (!stored) {
      if (marshalled) {
        freeMarshalledRowSet(arena, layout, marshalled.rowsetPtr);
      } else {
        releaseWasmRowSet(rowSet);
      }
    }
  }
}

function isLeftSemiAntiJoin(stage) {
  return stage.joinType === 'left_semi' ||
    stage.joinType === 'left_anti' ||
    stage.joinType === 'left-semi' ||
    stage.joinType === 'left-anti';
}

function isOuterJoin(stage) {
  return stage.joinType === 'left' || stage.joinType === 'right';
}

function finalizeSemiAntiJoinState(state) {
  if (state.semiAntiFinalized) {
    return;
  }
  const ok = state.dispatch(
    BigInt(state.leftTable),
    BigInt(state.rightTable),
    0n,
    0n,
    BigInt(state.pairBuffer),
    BigInt(state.leftStore.dataPtr()),
    BigInt(state.rightStore.dataPtr()),
    BigInt(state.leftStore.batches.length),
    JoinOp.FINALIZE);
  if (!ok) {
    throw new Error('join semi/anti finalize failed');
  }
  state.semiAntiFinalized = true;
}

// Outer join: after both sides are consumed, emit own-side rows that never
// matched, with kNullRowId on the opposite side. jt_dispatch leaves every pair
// in canonical (left_id, right_id) order, including RIGHT joins.
function finalizeOuterJoinState(state) {
  if (state.outerFinalized) {
    return;
  }
  const ok = state.dispatch(
    BigInt(state.leftTable),
    BigInt(state.rightTable),
    0n,
    0n,
    BigInt(state.pairBuffer),
    BigInt(state.leftStore.dataPtr()),
    BigInt(state.rightStore.dataPtr()),
    0n,
    JoinOp.FINALIZE);
  if (!ok) {
    throw new Error('join outer finalize failed');
  }
  state.outerFinalized = true;
}

function drainJoinPairs(state, maxRows = 1024, asWasm = false) {
  const pairLayout = state.layout.pairBuffer;
  const dv = state.arena.view();
  const count = Number(dv.getBigInt64(state.pairBuffer + pairLayout.count, true));
  if (state.materializeCursor >= count) {
    return [];
  }
  const batch = drainMaterializedBatch(
    state, maxRows, undefined, undefined, asWasm);
  if (state.materializeCursor >= count) {
    state.arena.view().setBigInt64(
      state.pairBuffer + pairLayout.count, 0n, true);
    state.materializeCursor = 0;
  }
  return [batch];
}

function finishJoinState(state) {
  if (state.finalized) {
    return;
  }
  state.finalized = true;
  state.dispatch(
    BigInt(state.leftTable),
    BigInt(state.rightTable),
    0n,
    0n,
    BigInt(state.pairBuffer),
    0n,
    0n,
    0n,
    JoinOp.DESTROY);
  // Reclaim the build-side row store so it doesn't linger for the rest of the
  // query (upstream joins finish before the final one peaks).
  state.leftStore.freeMarshalled();
  state.rightStore.freeMarshalled();
}

function createCrossJoinState(kernel, layout, stage) {
  const exports = kernel.instance.exports;
  const dispatch = exports.xj_dispatch;
  const materialize = exports.jt_materialize;
  if (typeof dispatch !== 'function' || typeof materialize !== 'function') {
    throw new Error('cross join kernel is missing an entry');
  }

  const arena = kernel.holder.arena;
  const pairBuffer = arena.alloc(layout.pairBuffer.size, 8);
  new Uint8Array(arena.memory.buffer, pairBuffer, layout.pairBuffer.size).fill(0);
  return {
    kernel,
    layout,
    stage,
    dispatch,
    materialize,
    arena,
    pairBuffer,
    leftStore: new WasmRowStore(arena, layout),
    rightStore: new WasmRowStore(arena, layout),
    materializeCursor: 0,
    rightRows: 0,
    finalized: false,
  };
}

function stableRowSetForStore(state, rowSet) {
  const { arena, layout } = state;
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  if (wasm?.pinned) {
    return { rowsetPtr: wasm.rowsetPtr, ownedPtr: 0, releaseInput: false };
  }
  const marshalled = marshalRowSet(
    arena, layout, batch.columns, batch.rowCount, false, selection);
  return {
    rowsetPtr: marshalled.rowsetPtr,
    ownedPtr: marshalled.rowsetPtr,
    releaseInput: true,
  };
}

function updateCrossRightState(state, rowSet) {
  const { rowsetPtr, ownedPtr, releaseInput } = stableRowSetForStore(state, rowSet);
  state.rightStore.push(rowSet.batch, rowsetPtr, ownedPtr);
  if (releaseInput) {
    releaseWasmRowSet(rowSet);
  }
  state.rightRows += rowSet.batch.rowCount;
}

function updateCrossLeftState(state, rowSet) {
  if (state.rightRows === 0) {
    releaseWasmRowSet(rowSet);
    return;
  }
  const { rowsetPtr, ownedPtr, releaseInput } = stableRowSetForStore(state, rowSet);
  const batchIdx = state.leftStore.push(rowSet.batch, rowsetPtr, ownedPtr);
  const batchPtr = state.leftStore.dataPtr() + batchIdx * state.layout.rowset.size;
  try {
    const ok = state.dispatch(
      BigInt(batchPtr),
      BigInt(batchIdx),
      BigInt(state.rightStore.dataPtr()),
      BigInt(state.rightStore.batches.length),
      BigInt(state.pairBuffer),
      CrossJoinOp.EMIT);
    if (!ok) {
      throw new Error('cross join kernel update failed');
    }
  } finally {
    if (releaseInput) {
      releaseWasmRowSet(rowSet);
    }
  }
}

function drainCrossJoinPairs(state, maxRows = 1024, asWasm = false) {
  const pairLayout = state.layout.pairBuffer;
  const dv = state.arena.view();
  const count = Number(dv.getBigInt64(state.pairBuffer + pairLayout.count, true));
  if (state.materializeCursor >= count) {
    return [];
  }
  const batch = drainMaterializedBatch(
    state, maxRows, undefined, undefined, asWasm);
  if (state.materializeCursor >= count) {
    state.arena.view().setBigInt64(
      state.pairBuffer + pairLayout.count, 0n, true);
    state.materializeCursor = 0;
  }
  return [batch];
}

function finishCrossJoinState(state) {
  if (state.finalized) {
    return;
  }
  state.finalized = true;
  state.dispatch(
    0n,
    0n,
    0n,
    0n,
    BigInt(state.pairBuffer),
    CrossJoinOp.DESTROY);
  state.leftStore.freeMarshalled();
  state.rightStore.freeMarshalled();
}

function outputShapeFromRowSets(rowSets) {
  const first = rowSets.find(rowSet => rowSet.batch.columns.length > 0);
  return first ? first.batch.columns.map(col => ({
    name: col.name,
    type: col.type,
  })) : [];
}

function emptyBatchForRowSets(rowSets) {
  return {
    rowCount: 0,
    columns: outputShapeFromRowSets(rowSets).map(col => ({
      name: col.name,
      type: col.type,
      values: [],
    })),
  };
}

function selectedRowCount(rowSets) {
  let count = 0;
  for (const rowSet of rowSets) {
    const { batch, selection } = rowSet;
    if (!selection) {
      count += batch.rowCount;
      continue;
    }
    for (let row = 0; row < batch.rowCount; ++row) {
      if (rowSelected(selection, row)) ++count;
    }
  }
  return count;
}

function stableSortRowSetForStore(arena, layout, rowSet) {
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  if (wasm?.pinned) {
    return { rowsetPtr: wasm.rowsetPtr, ownedPtr: 0, releaseInput: false };
  }
  const marshalled = marshalRowSet(
    arena, layout, batch.columns, batch.rowCount, false, selection);
  return {
    rowsetPtr: marshalled.rowsetPtr,
    ownedPtr: marshalled.rowsetPtr,
    releaseInput: true,
  };
}

function runSortKernelRowSets(kernel, layout, rowSets, radixKeys, limit) {
  const arena = kernel.holder.arena;

  const n = selectedRowCount(rowSets);
  const keyCount = radixKeys.length;
  const nullable = !!kernel.nullable;
  const workStride = radixKeys.some(key => key.isString) ? 4 : 1;
  const store = new WasmRowStore(arena, layout);
  const storeBatchIndexes = new Array(rowSets.length);
  const releaseInputs = [];

  for (let b = 0; b < rowSets.length; ++b) {
    const rowSet = rowSets[b];
    const stable = stableSortRowSetForStore(arena, layout, rowSet);
    if (stable.releaseInput) {
      releaseInputs.push(rowSet);
    }
    storeBatchIndexes[b] = store.push(
      rowSet.batch, stable.rowsetPtr, stable.ownedPtr);
  }

  if (n === 0) {
    for (const rowSet of releaseInputs) {
      releaseWasmRowSet(rowSet);
    }
    store.freeMarshalled();
    return emptyBatchForRowSets(rowSets);
  }
  const keep = (limit != null && limit < n)
    ? Math.max(Number(limit), 0)
    : n;
  if (keep === 0) {
    for (const rowSet of releaseInputs) {
      releaseWasmRowSet(rowSet);
    }
    store.freeMarshalled();
    return emptyBatchForRowSets(rowSets);
  }

  // Allocate everything up front (a mid-write grow would detach views).
  const rowIdsPtr = arena.alloc(Math.max(n, 1) * 8, 8);
  const workPtr = arena.alloc(Math.max(n, 1) * workStride * 8, 8);
  const countsPtr = arena.alloc((nullable ? 257 : 256) * 4, 4);
  const descsPtr = arena.alloc(Math.max(keyCount, 1), 1);
  const nullsFirstsPtr = arena.alloc(Math.max(keyCount, 1), 1);
  const outRowSetPtr = arena.alloc(layout.rowset.size, 8);
  let outRowSetTransferred = false;

  const dv = arena.view();
  for (let k = 0; k < keyCount; ++k) {
    const key = radixKeys[k];
    dv.setUint8(descsPtr + k, key.desc ? 1 : 0);
    dv.setUint8(nullsFirstsPtr + k, key.nullsFirst ? 1 : 0);
  }

  let out = 0;
  for (let b = 0; b < rowSets.length; ++b) {
    const rowSet = rowSets[b];
    const { batch, selection } = rowSet;
    for (let row = 0; row < batch.rowCount; ++row) {
      if (!rowSelected(selection, row)) continue;
      dv.setBigInt64(rowIdsPtr + out * 8, makeRowId(storeBatchIndexes[b], row), true);
      ++out;
    }
  }

  try {
    // memory64: every pointer/count is an i64 param.
    const out = Number(kernel.fn(
      BigInt(store.dataPtr()),
      BigInt(rowIdsPtr),
      BigInt(workPtr),
      BigInt(countsPtr),
      BigInt(n),
      BigInt(descsPtr),
      BigInt(nullsFirstsPtr),
      1,
      0n,
      BigInt(keep),
      BigInt(outRowSetPtr)));
    if (out <= 0) {
      return emptyBatchForRowSets(rowSets);
    }
    outRowSetTransferred = true;
    return makeWasmOwnedBatch(
      { arena, layout },
      outRowSetPtr,
      outputShapeFromRowSets(rowSets));
  } finally {
    arena.free(rowIdsPtr);
    arena.free(workPtr);
    arena.free(countsPtr);
    arena.free(descsPtr);
    arena.free(nullsFirstsPtr);
    if (!outRowSetTransferred) {
      arena.free(outRowSetPtr);
    }
    for (const rowSet of releaseInputs) {
      releaseWasmRowSet(rowSet);
    }
    store.freeMarshalled();
  }
}

function runRadixSortRowSets(kernel, layout, rowSets, radixKeys, limit) {
  try {
    return runSortKernelRowSets(kernel, layout, rowSets, radixKeys, limit);
  } finally {
    for (const rowSet of rowSets) {
      releaseWasmRowSet(rowSet);
    }
  }
}

// Drive the radix composite sort kernel. The kernel sorts a row-id permutation
// in place and reads key values directly from marshaled TRowSet storage,
// matching the native sort path.
export function runRadixSort(kernel, layout, batch, selection, radixKeys, limit) {
  return runRadixSortRowSets(
    kernel, layout, [{ batch, selection: selection || null }], radixKeys, limit);
}

const TaskResult = {
  OK: 'OK',
  NEED_DATA: 'NEED_DATA',
  BLOCKED_OUTPUT: 'BLOCKED_OUTPUT',
  FINISHED: 'FINISHED',
};

const FetchResult = {
  OK: 'OK',
  NO_DATA: 'NO_DATA',
  FINISHED: 'FINISHED',
};

const ConnectionKind = {
  OneToOne: 'one-to-one',
  Gather: 'gather',
  HashShuffle: 'hash-shuffle',
  Broadcast: 'broadcast',
};

class OneToOneConnection {
  constructor(capacity = 1) {
    this.kind = ConnectionKind.OneToOne;
    this.capacity = capacity;
    this.queue = [];
    this.finished = false;
    this.stats = {
      pushed: 0,
      popped: 0,
      finished: 0,
      blockedPush: 0,
      emptyFetch: 0,
      finishedFetch: 0,
    };
  }

  canPush() {
    return this.queue.length < this.capacity;
  }

  push(rowSet) {
    if (!this.canPush()) {
      ++this.stats.blockedPush;
      return false;
    }
    this.queue.push(rowSet);
    ++this.stats.pushed;
    return true;
  }

  finish() {
    if (!this.finished) {
      this.finished = true;
      ++this.stats.finished;
    }
  }

  fetch() {
    if (this.queue.length > 0) {
      ++this.stats.popped;
      return { result: FetchResult.OK, rowSet: this.queue.shift() };
    }
    if (this.finished) {
      ++this.stats.finishedFetch;
      return { result: FetchResult.FINISHED, rowSet: null };
    }
    ++this.stats.emptyFetch;
    return { result: FetchResult.NO_DATA, rowSet: null };
  }
}

class UnsupportedConnection {
  constructor(kind) {
    this.kind = kind;
    this.stats = {
      pushed: 0,
      popped: 0,
      finished: 0,
      blockedPush: 0,
      emptyFetch: 0,
      finishedFetch: 0,
    };
  }

  unsupported() {
    throw new Error(`browser scheduler connection is not implemented: ${this.kind}`);
  }

  canPush() { return this.unsupported(); }
  push() { return this.unsupported(); }
  finish() { return this.unsupported(); }
  fetch() { return this.unsupported(); }
}

class GatherConnection extends UnsupportedConnection {
  constructor() {
    super(ConnectionKind.Gather);
  }
}

class HashShuffleConnection extends UnsupportedConnection {
  constructor() {
    super(ConnectionKind.HashShuffle);
  }
}

class BroadcastConnection extends UnsupportedConnection {
  constructor() {
    super(ConnectionKind.Broadcast);
  }
}

function createConnection(kind = ConnectionKind.OneToOne, options = {}) {
  switch (kind) {
    case ConnectionKind.OneToOne:
      return new OneToOneConnection(options.capacity || 1);
    case ConnectionKind.Gather:
      return new GatherConnection();
    case ConnectionKind.HashShuffle:
      return new HashShuffleConnection();
    case ConnectionKind.Broadcast:
      return new BroadcastConnection();
    default:
      throw new Error(`unknown browser scheduler connection kind: ${kind}`);
  }
}

class SchedulerNode {
  constructor(task, label) {
    this.task = task;
    this.label = label;
    this.inbound = [];
    this.outbound = [];
    this.elapsedMs = 0;
    this.rows = 0;
  }
}

class SingleThreadedScheduler {
  constructor(roots) {
    this.roots = Array.isArray(roots) ? roots : [roots];
    this.ready = [];
    this.scheduled = new Set();
    this.stats = {
      scheduled: 0,
      popped: 0,
      executed: 0,
      needData: 0,
      blockedOutput: 0,
      ok: 0,
      finished: 0,
    };
  }

  async run() {
    for (const root of this.roots) {
      this.schedule(root);
    }
    while (this.ready.length > 0) {
      const node = this.ready.shift();
      this.scheduled.delete(node);
      ++this.stats.popped;

      const started = nowMs();
      const state = await node.task.execute();
      node.elapsedMs += nowMs() - started;
      node.rows = node.task.rows || node.rows || 0;
      ++this.stats.executed;

      if (state === TaskResult.NEED_DATA) {
        ++this.stats.needData;
        this.scheduleInput(node);
      } else if (state === TaskResult.BLOCKED_OUTPUT || state === TaskResult.FINISHED) {
        if (state === TaskResult.BLOCKED_OUTPUT) {
          ++this.stats.blockedOutput;
        } else {
          ++this.stats.finished;
        }
        this.scheduleOutput(node);
      } else if (state === TaskResult.OK) {
        ++this.stats.ok;
        this.schedule(node);
        this.scheduleOutput(node);
      }
    }
  }

  schedule(node) {
    if (!node || this.scheduled.has(node)) {
      return;
    }
    this.ready.push(node);
    this.scheduled.add(node);
    ++this.stats.scheduled;
  }

  scheduleInput(node) {
    for (const edge of node.inbound) {
      this.schedule(edge.src);
    }
  }

  scheduleOutput(node) {
    for (const edge of node.outbound) {
      this.schedule(edge.dst);
    }
  }
}

function connect(src, dst, connection = new OneToOneConnection(1), options = {}) {
  const edge = { src, dst, connection, persistent: options.persistent === true };
  src.outbound.push(edge);
  dst.inbound.push(edge);
  return edge;
}

function assertStreamingWasmEdge(edge) {
  if (edge.connection.kind !== ConnectionKind.OneToOne || edge.connection.capacity !== 1) {
    throw new Error('streaming wasm rowsets require a one-slot one-to-one edge');
  }
}

function canConsumeWasmOutput(task) {
  if (!task) {
    return false;
  }
  if (task instanceof SinkTask) {
    return true;
  }
  const kind = task.stage?.kind;
  return kind === 'aggregate' || kind === 'join' || kind === 'cross-join';
}

function downstreamConsumesWasm(node) {
  return canConsumeWasmOutput(node?.outbound?.[0]?.dst?.task);
}

class SourceTask {
  constructor(stage, layout, shared, readSourceBatches, onProgress) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.readSourceBatches = readSourceBatches;
    this.onProgress = onProgress;
    this.iterator = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) {
      return TaskResult.FINISHED;
    }
    const out = this.outbound.connection;
    if (!out.canPush()) {
      return TaskResult.BLOCKED_OUTPUT;
    }
    if (!this.iterator) {
      this.iterator = this.readSourceBatches(this.stage, this.onProgress)[Symbol.asyncIterator]();
    }
    const next = await this.iterator.next();
    if (next.done) {
      out.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }
    const batch = pinSourceBatch(
      next.value, this.shared.arena, this.layout);
    this.rows += batch.rowCount;
    out.push({ batch, selection: null });
    return TaskResult.OK;
  }

  get outbound() {
    return this.node.outbound[0];
  }
}

class FilterTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.writer = new RowSetWriter(shared.arena, layout);
    this.kernel = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const outEdge = this.node.outbound[0];
    const output = outEdge.connection;
    if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;

    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.FINISHED) {
      output.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }
    if (!this.kernel) {
      this.kernel = await instantiateKernel(this.stage.wasm, '<kernel>', this.shared);
    }
    assertStreamingWasmEdge(outEdge);
    const reusesInput = reusableFilterWasm(
      fetched.rowSet, outEdge.persistent) !== null;
    const releaseInput = reusesInput ? null : takeRowSetRelease(fetched.rowSet);
    let selection = null;
    try {
      selection = runFilter(
        this.kernel,
        this.layout,
        fetched.rowSet,
        this.writer,
        { pinned: outEdge.persistent });
    } finally {
      if (releaseInput) {
        releaseInput();
      }
    }
    this.rows += fetched.rowSet.batch.rowCount;
    output.push({ batch: fetched.rowSet.batch, selection });
    return TaskResult.OK;
  }
}

class ProjectTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.kernel = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const output = this.node.outbound[0].connection;
    if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;

    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.FINISHED) {
      output.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }

    const hasComputed = this.stage.output.some(col => col.source === 'computed');
    if (hasComputed) {
      if (!this.stage.wasm) {
        throw new Error('project stage is missing wasm kernel');
      }
      if (!this.kernel) {
        this.kernel = await instantiateKernel(this.stage.wasm, '<project>', this.shared);
      }
    }
    if (fetched.rowSet.batch.wasm && !fetched.rowSet.batch.wasm.pinned) {
      assertStreamingWasmEdge(this.node.inbound[0]);
    }
    const outRowSet = runProject(
      this.kernel,
      this.layout,
      fetched.rowSet,
      this.stage.output,
      this.shared.arena);
    this.rows += outRowSet.batch.rowCount;
    output.push(outRowSet);
    return TaskResult.OK;
  }
}

class AggregateTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.kernel = null;
    this.state = null;
    this.pending = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const output = this.node.outbound[0].connection;

    if (this.pending) {
      if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
      output.push({ batch: this.pending, selection: null });
      output.finish();
      this.pending = null;
      this.done = true;
      return TaskResult.FINISHED;
    }

    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.FINISHED) {
      if (!this.state) {
        if (!this.kernel) {
          this.kernel = await instantiateKernel(
            this.stage.wasm, 'agg_dispatch', this.shared);
        }
        this.state = createAggregateState(this.kernel, this.layout, this.stage);
      }
      this.pending = finishAggregateState(
        this.state, downstreamConsumesWasm(this.node));
      return this.execute();
    }

    if (!this.kernel) {
      this.kernel = await instantiateKernel(
        this.stage.wasm, 'agg_dispatch', this.shared);
      this.state = createAggregateState(this.kernel, this.layout, this.stage);
    }
    if (fetched.rowSet.batch.wasm && !fetched.rowSet.batch.wasm.pinned) {
      assertStreamingWasmEdge(this.node.inbound[0]);
    }
    try {
      updateAggregateState(this.state, fetched.rowSet);
      this.rows += fetched.rowSet.batch.rowCount;
    } finally {
      releaseWasmRowSet(fetched.rowSet);
    }
    return TaskResult.OK;
  }
}

class JoinTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.kernel = null;
    this.state = null;
    this.ready = [];
    this.done = false;
    this.leftDone = false;
    this.rightDone = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const output = this.node.outbound[0].connection;
    const asWasm = downstreamConsumesWasm(this.node);

    if (this.ready.length > 0) {
      if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
      output.push({ batch: this.ready.shift(), selection: null });
      return TaskResult.OK;
    }

    if (!this.state) {
      if (!this.kernel) {
        this.kernel = await instantiateKernel(this.stage.wasm, 'jt_dispatch', this.shared);
      }
      this.state = createJoinState(this.kernel, this.layout, this.stage);
    }

    const drained = drainJoinPairs(this.state, 1024, asWasm);
    if (drained.length > 0) {
      this.ready.push(...drained);
      return this.execute();
    }

    const progressed = this.pullOneInputBatch();
    if (progressed) {
      this.ready.push(...drainJoinPairs(this.state, 1024, asWasm));
      if (this.ready.length > 0) {
        return this.execute();
      }
      return TaskResult.OK;
    }

    if (this.leftDone && this.rightDone) {
      if (isLeftSemiAntiJoin(this.stage) && !this.state.semiAntiFinalized) {
        finalizeSemiAntiJoinState(this.state);
        this.ready.push(...drainJoinPairs(this.state, 1024, asWasm));
        if (this.ready.length > 0) {
          return this.execute();
        }
      }
      if (isOuterJoin(this.stage) && !this.state.outerFinalized) {
        finalizeOuterJoinState(this.state);
        this.ready.push(...drainJoinPairs(this.state, 1024, asWasm));
        if (this.ready.length > 0) {
          return this.execute();
        }
      }
      finishJoinState(this.state);
      output.finish();
      this.done = true;
      return TaskResult.FINISHED;
    }
    return TaskResult.NEED_DATA;
  }

  pullOneInputBatch() {
    if (isLeftSemiAntiJoin(this.stage)) {
      return this.pullOneSemiAntiInputBatch();
    }
    if (isInnerJoin(this.stage)) {
      return this.pullOneInnerStreamingBatch(
        downstreamConsumesWasm(this.node));
    }
    // Outer joins keep both sides buffered: the finalize scan needs the build
    // table plus the stored rows to emit unmatched padding.
    if (!this.leftDone) {
      const fetched = this.node.inbound[0].connection.fetch();
      if (fetched.result === FetchResult.OK) {
        updateJoinState(this.state, 0, fetched.rowSet);
        this.rows += fetched.rowSet.batch.rowCount;
        return true;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.leftDone = true;
        return true;
      }
    }

    if (!this.rightDone) {
      const fetched = this.node.inbound[1].connection.fetch();
      if (fetched.result === FetchResult.OK) {
        updateJoinState(this.state, 1, fetched.rowSet);
        this.rows += fetched.rowSet.batch.rowCount;
        return true;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.rightDone = true;
        return true;
      }
    }

    return false;
  }

  // Inner join: build the left side to completion (stored + hash table), then
  // STREAM the right (probe) side — never storing it — so peak memory is bounded
  // by the build side rather than both inputs. The probe emits pairs that are
  // materialized against the live right batch inside streamJoinRightBatch.
  pullOneInnerStreamingBatch(asWasm) {
    if (!this.leftDone) {
      const fetched = this.node.inbound[0].connection.fetch();
      if (fetched.result === FetchResult.OK) {
        updateJoinState(this.state, 0, fetched.rowSet);
        this.rows += fetched.rowSet.batch.rowCount;
        return true;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.leftDone = true;
        return true;
      }
      return false;
    }

    if (!this.rightDone) {
      const fetched = this.node.inbound[1].connection.fetch();
      if (fetched.result === FetchResult.OK) {
        this.ready.push(...streamJoinRightBatch(
          this.state, fetched.rowSet, asWasm));
        this.rows += fetched.rowSet.batch.rowCount;
        return true;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.rightDone = true;
        return true;
      }
    }

    return false;
  }

  pullOneSemiAntiInputBatch() {
    if (!this.leftDone) {
      const fetched = this.node.inbound[0].connection.fetch();
      if (fetched.result === FetchResult.OK) {
        updateJoinState(this.state, 0, fetched.rowSet);
        this.rows += fetched.rowSet.batch.rowCount;
        return true;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.leftDone = true;
        return true;
      }
      return false;
    }

    if (!this.rightDone) {
      const fetched = this.node.inbound[1].connection.fetch();
      if (fetched.result === FetchResult.OK) {
        updateJoinState(this.state, 1, fetched.rowSet);
        this.rows += fetched.rowSet.batch.rowCount;
        return true;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.rightDone = true;
        return true;
      }
    }

    return false;
  }
}

// Cross product of two inputs (in practice: a scalar-subquery broadcast, the
// right side is one row). The kernel emits row-id pairs; jt_materialize gathers
// the left ++ right output columns.
class CrossJoinTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.kernel = null;
    this.state = null;
    this.ready = [];
    this.leftDone = false;
    this.rightDone = false;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const output = this.node.outbound[0].connection;
    const asWasm = downstreamConsumesWasm(this.node);

    if (this.ready.length > 0) {
      if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
      const batch = this.ready.shift();
      output.push({ batch, selection: null });
      this.rows += batch.rowCount;
      return TaskResult.OK;
    }

    if (!this.state) {
      if (!this.stage.wasm) {
        throw new Error('cross join stage is missing wasm kernel');
      }
      if (!this.kernel) {
        this.kernel = await instantiateKernel(this.stage.wasm, 'xj_dispatch', this.shared);
      }
      this.state = createCrossJoinState(this.kernel, this.layout, this.stage);
    }

    const drained = drainCrossJoinPairs(this.state, 1024, asWasm);
    if (drained.length > 0) {
      this.ready.push(...drained);
      return this.execute();
    }

    if (!this.rightDone) {
      const fetched = this.node.inbound[1].connection.fetch();
      if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
      if (fetched.result === FetchResult.OK) {
        updateCrossRightState(this.state, fetched.rowSet);
        return TaskResult.OK;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.rightDone = true;
        return TaskResult.OK;
      }
    }

    if (!this.leftDone) {
      const fetched = this.node.inbound[0].connection.fetch();
      if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
      if (fetched.result === FetchResult.OK) {
        updateCrossLeftState(this.state, fetched.rowSet);
        this.ready.push(...drainCrossJoinPairs(this.state, 1024, asWasm));
        if (this.ready.length > 0) {
          return this.execute();
        }
        return TaskResult.OK;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this.leftDone = true;
      }
    }

    finishCrossJoinState(this.state);
    output.finish();
    this.done = true;
    return TaskResult.FINISHED;
  }
}

class TopSortWasmState {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.limit = Math.max(Number(stage.limit || 0), 0);
    this.kernel = null;
    this.stateBatch = null;
  }

  async ensureKernels() {
    if (!this.stage.wasm || !Array.isArray(this.stage.radixKeys)) {
      throw new Error('top-sort stage is missing wasm kernel');
    }
    if (!this.kernel) {
      this.kernel = await instantiateKernel(
        this.stage.wasm,
        'qdb_top_sort_update',
        this.shared);
    }
  }

  async add(rowSet) {
    await this.ensureKernels();
    if (!this.stateBatch) {
      this.stateBatch = emptyBatchForRowSets([rowSet]);
    }
    if (this.limit <= 0) {
      releaseWasmRowSet(rowSet);
      return;
    }

    const n = selectedRowCount([rowSet]);
    if (n === 0) {
      releaseWasmRowSet(rowSet);
      return;
    }

    const arena = this.shared.arena;
    const oldStateBatch = this.stateBatch;
    const stateWasm = oldStateBatch?.wasm;
    const stateRowSet = stateWasm
      ? { rowsetPtr: stateWasm.rowsetPtr, ownedPtr: 0 }
      : marshalRowSet(
        arena,
        this.layout,
        oldStateBatch.columns,
        oldStateBatch.rowCount,
        false,
        null);
    const batchRowSet = stableSortRowSetForStore(arena, this.layout, rowSet);
    const pickCapacity = Math.min(
      this.limit, this.stateBatch.rowCount + n);
    const workStride = this.stage.radixKeys.some(key => key.isString) ? 4 : 1;
    const rowIdsPtr = arena.alloc(Math.max(n, pickCapacity, 1) * 8, 8);
    const workPtr = arena.alloc(Math.max(n, 1) * workStride * 8, 8);
    const countsPtr = arena.alloc((this.stage.radixNullable ? 257 : 256) * 4, 4);
    const pickSrcPtr = arena.alloc(Math.max(pickCapacity, 1), 1);
    const pickIdxPtr = arena.alloc(Math.max(pickCapacity, 1) * 4, 4);
    const outRowSetPtr = arena.alloc(this.layout.rowset.size, 8);
    let outRowSetTransferred = false;

    const dv = arena.view();
    let outRow = 0;
    const { batch, selection } = rowSet;
    for (let row = 0; row < batch.rowCount; ++row) {
      if (!rowSelected(selection, row)) continue;
      dv.setBigInt64(rowIdsPtr + outRow * 8, makeRowId(0, row), true);
      ++outRow;
    }

    try {
      const out = Number(this.kernel.fn(
        BigInt(stateRowSet.rowsetPtr),
        BigInt(batchRowSet.rowsetPtr),
        BigInt(rowIdsPtr),
        BigInt(workPtr),
        BigInt(countsPtr),
        BigInt(n),
        BigInt(pickSrcPtr),
        BigInt(pickIdxPtr),
        BigInt(this.limit),
        BigInt(outRowSetPtr)));
      if (out <= 0) {
        this.stateBatch = emptyBatchForRowSets([rowSet]);
      } else {
        outRowSetTransferred = true;
        this.stateBatch = makeWasmOwnedBatch(
          { arena, layout: this.layout },
          outRowSetPtr,
          outputShapeFromRowSets([rowSet]));
      }
    } finally {
      if (stateRowSet.ownedPtr) {
        freeMarshalledRowSet(arena, this.layout, stateRowSet.ownedPtr);
      }
      if (oldStateBatch?.wasm) {
        releaseWasmRowSet({ batch: oldStateBatch });
      }
      if (batchRowSet.ownedPtr) {
        freeMarshalledRowSet(arena, this.layout, batchRowSet.ownedPtr);
      }
      releaseWasmRowSet(rowSet);
      if (!outRowSetTransferred) {
        arena.free(outRowSetPtr);
      }
      arena.free(rowIdsPtr);
      arena.free(workPtr);
      arena.free(countsPtr);
      arena.free(pickSrcPtr);
      arena.free(pickIdxPtr);
    }
  }

  finish() {
    return this.stateBatch || { rowCount: 0, columns: [] };
  }
}

class SortTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.topSort = stage.kind === 'top-sort'
      ? new TopSortWasmState(stage, layout, shared)
      : null;
    this.inputs = [];
    this.pending = null;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const output = this.node.outbound[0].connection;

    if (this.pending) {
      if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
      output.push({ batch: this.pending, selection: null });
      output.finish();
      this.pending = null;
      this.done = true;
      return TaskResult.FINISHED;
    }

    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.OK) {
      if (this.topSort) {
        await this.topSort.add(fetched.rowSet);
      } else {
        this.inputs.push(fetched.rowSet);
      }
      this.rows += fetched.rowSet.batch.rowCount;
      return TaskResult.OK;
    }

    if (this.topSort) {
      this.pending = this.topSort.finish();
      return this.execute();
    }

    if (!this.stage.wasm || !Array.isArray(this.stage.radixKeys)) {
      throw new Error(`${this.stage.kind} stage is missing wasm radix sort kernel`);
    }
    const radixNullable = !!this.stage.radixNullable;
    const kernel = await instantiateKernel(
      this.stage.wasm,
      'qdb_sort_run',
      this.shared);
    kernel.nullable = radixNullable;
    this.pending = runRadixSortRowSets(
      kernel, this.layout, this.inputs, this.stage.radixKeys, null);
    return this.execute();
  }
}

class SinkTask {
  constructor(limit, layout, shared) {
    this.limit = limit;
    this.layout = layout;
    this.shared = shared;
    this.rowSets = [];
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const input = this.node.inbound[0].connection;
    const fetched = input.fetch();
    if (fetched.result === FetchResult.NO_DATA) return TaskResult.NEED_DATA;
    if (fetched.result === FetchResult.FINISHED) {
      this.done = true;
      return TaskResult.FINISHED;
    }
    this.rowSets.push(fetched.rowSet);
    this.rows += fetched.rowSet.batch.rowCount;
    return TaskResult.OK;
  }

  result() {
    return materializeRowSets(
      this.rowSets, this.limit, this.layout, this.shared.arena);
  }
}

function makeNode(task, label) {
  const node = new SchedulerNode(task, label);
  task.node = node;
  return node;
}

function taskNodeForStage(stage, layout, readSourceBatches, onProgress, shared) {
  if (stage.kind === 'source') {
    return makeNode(
      new SourceTask(stage, layout, shared, readSourceBatches, onProgress || null),
      stage.kind);
  }
  if (stage.kind === 'filter') {
    return makeNode(new FilterTask(stage, layout, shared), stage.kind);
  }
  if (stage.kind === 'project') {
    return makeNode(new ProjectTask(stage, layout, shared), stage.kind);
  }
  if (stage.kind === 'aggregate') {
    return makeNode(new AggregateTask(stage, layout, shared), stage.kind);
  }
  if (stage.kind === 'join') {
    return makeNode(new JoinTask(stage, layout, shared), stage.kind);
  }
  if (stage.kind === 'cross-join') {
    return makeNode(new CrossJoinTask(stage, layout, shared), stage.kind);
  }
  if (stage.kind === 'sort' || stage.kind === 'top-sort') {
    return makeNode(new SortTask(stage, layout, shared), stage.kind);
  }
  throw new Error(`unsupported exec stage: ${stage.kind}`);
}

function buildScheduledGraph(exec, readSourceBatches, options, layout, shared) {
  const nodesById = new Map();
  const nodes = [];
  for (const spec of exec.nodes || []) {
    if (spec.kind === 'limit') {
      continue;
    }
    const node = taskNodeForStage(
      spec, layout, readSourceBatches, options.onProgress, shared);
    nodesById.set(Number(spec.id), node);
    nodes.push(node);
  }
  for (const edge of exec.edges || []) {
    const src = nodesById.get(Number(edge.from));
    const dst = nodesById.get(Number(edge.to));
    if (!src || !dst) {
      throw new Error('exec graph edge references an unknown node');
    }
    connect(
      src,
      dst,
      createConnection(edge.kind || ConnectionKind.OneToOne),
      { persistent: dst.task?.stage?.kind === 'join' });
  }
  const root = nodesById.get(Number(exec.root));
  if (!root) {
    throw new Error('exec graph root references an unknown node');
  }
  const limit = exec.limit
    ? { limit: Number(exec.limit.limit), offset: Number(exec.limit.offset || 0) }
    : null;
  const sink = makeNode(new SinkTask(limit, layout, shared), 'materialize');
  nodes.push(sink);
  connect(root, sink, createConnection(ConnectionKind.OneToOne));
  const roots = nodes.filter(node => node.inbound.length === 0);
  return { roots, sink, nodes };
}

export async function executeBrowserPipelineScheduled(exec, readSourceBatches, options = {}) {
  if (!exec || exec.supported !== true) {
    throw new Error(exec?.reason || 'pipeline is not supported for browser execution');
  }
  if (exec.embedWasm !== true) {
    throw new Error('exec plan was produced without embedded wasm kernels');
  }

  const layout = exec.layout;
  if (!Array.isArray(exec.nodes)) {
    throw new Error('exec plan is missing graph nodes');
  }
  const shared = createSharedMemory(layout);
  const { roots, sink, nodes } =
    buildScheduledGraph(exec, readSourceBatches, options, layout, shared);
  const scheduler = new SingleThreadedScheduler(roots);
  await scheduler.run();

  const result = sink.task.result();
  result.timings = nodes.map(node => ({
    stage: node.label,
    rows: node.rows,
    elapsedMs: node.elapsedMs,
  }));
  result.scheduler = scheduler.stats;
  result.connections = nodes.flatMap(node =>
    node.outbound.map(edge => ({
      from: edge.src.label,
      to: edge.dst.label,
      kind: edge.connection.kind,
      ...edge.connection.stats,
    })));
  return result;
}

function batchHasJsValues(batch) {
  return batch.columns.every(column =>
    Object.prototype.hasOwnProperty.call(column, 'values'));
}

function finalMaterializeBatch(rowSet, layout, arena) {
  const batch = rowSet.batch;
  if (batchHasJsValues(batch)) {
    return batch;
  }
  if (!batch.wasm) {
    throw new Error('rowset has no JS values and no wasm handle');
  }
  return materializeWasmRowSet(arena, layout, batch.columns, batch.wasm.rowsetPtr);
}

function releaseWasmRowSets(rowSets, start) {
  for (let i = start; i < rowSets.length; ++i) {
    releaseWasmRowSet(rowSets[i]);
  }
}

function materializeRowSets(rowSets, limit, layout, arena) {
  const first = rowSets.find(rowSet => rowSet.batch.columns.length > 0);
  const columns = first ? first.batch.columns.map(col => col.name) : [];
  const rows = [];
  const offset = limit ? limit.offset : 0;
  const max = limit ? limit.limit : Infinity;
  let skipped = 0;

  if (max <= 0) {
    releaseWasmRowSets(rowSets, 0);
    return { columns, rows };
  }

  for (let setIndex = 0; setIndex < rowSets.length; ++setIndex) {
    const rowSet = rowSets[setIndex];
    const { selection } = rowSet;
    let reachedLimit = false;
    try {
      const batch = finalMaterializeBatch(rowSet, layout, arena);
      for (let i = 0; i < batch.rowCount; ++i) {
        if (!rowSelected(selection, i)) continue;
        if (skipped < offset) {
          ++skipped;
          continue;
        }
        if (rows.length >= max) {
          reachedLimit = true;
          break;
        }
        rows.push(batch.columns.map(col => formatValue(col.values[i])));
      }
    } finally {
      releaseWasmRowSet(rowSet);
    }
    if (reachedLimit) {
      releaseWasmRowSets(rowSets, setIndex + 1);
      return { columns, rows };
    }
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
