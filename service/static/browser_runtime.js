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

function copyNumericColumnFromMemory(memory, ptr, type, rowCount) {
  const Ctor = numericArrayConstructor(type);
  if (!Ctor) {
    throw new Error(`unsupported numeric column type: ${type}`);
  }
  return new Ctor(memory.buffer, ptr, rowCount).slice();
}

function makeRowId(batchIdx, rowIdx) {
  return (BigInt(batchIdx) << 32n) | BigInt(rowIdx >>> 0);
}

function rowIdBatch(id) {
  return Number(id >> 32n);
}

function rowIdRow(id) {
  return Number(id & 0xffffffffn);
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
      memBytes.set(selection.subarray(0, rowCount), this.selectionPtr);
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
    this.storePtr = 0;
    this.capacity = 0;
  }

  push(batch, rowsetPtr) {
    this.ensureCapacity(this.batches.length + 1);
    const idx = this.batches.length;
    this.batches.push(batch);
    const size = this.layout.rowset.size;
    const memBytes = this.arena.bytes();
    memBytes.copyWithin(
      this.storePtr + idx * size,
      rowsetPtr,
      rowsetPtr + size);
    return idx;
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
function attachBatchWasm(batch, rowsetPtr, { pinned, selection = null }) {
  batch.wasm = {
    rowsetPtr,
    pinned: pinned === true,
    hasSelection: selection !== null,
    selection,
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
  return wasm.hasSelection && wasm.selection === selection ? wasm : null;
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
    memBytes.set(selection.subarray(0, rowCount), selectionPtr);
    writePointer(dv, rowsetPtr + rsLayout.selection, selectionPtr);
  } else if (wantSelection) {
    writePointer(dv, rowsetPtr + rsLayout.selection, selectionPtr);
  }

  return { rowsetPtr, selectionPtr, columnsBase };
}

// Run a filter kernel over `batch` (columns in source order). Returns a
// Uint8Array selection where a nonzero byte marks a kept row.
export function runFilter(kernel, layout, batch, writer, options = {}) {
  const pinned = options.pinned === true;
  const { rowsetPtr, selectionPtr } = pinned
    ? marshalRowSet(
        kernel.holder.arena, layout, batch.columns, batch.rowCount, true)
    : writer.write(batch.columns, batch.rowCount, true);
  // memory64 entry: the rowset pointer is an i64 param.
  kernel.fn(BigInt(rowsetPtr));
  // Copy the selection out before the buffer can be reused.
  const selection = kernel.holder.arena.bytes()
    .slice(selectionPtr, selectionPtr + batch.rowCount);
  attachBatchWasm(batch, rowsetPtr, { pinned, selection });
  return selection;
}

// Run a project kernel over `batch`; `output` is the graph project's output plan.
// Returns the new columns array (source order preserved for the pipeline).
export function runProject(kernel, layout, batch, output, writer, state) {
  const arena = writer.arena;
  const rowCount = batch.rowCount;

  const computed = output.filter(col => col.source === 'computed');
  if (!state.outArrayPtr && computed.length) {
    state.outArrayPtr = arena.alloc(computed.length * 8, 8);
  }
  while (state.computedBufs.length < computed.length) {
    state.computedBufs.push({ ptr: 0, capacity: 0 });
  }
  const computedBufs = computed.map((col, i) => {
    const bytes = Math.max(rowCount, 1) * col.width;
    if (state.computedBufs[i].capacity < bytes) {
      state.computedBufs[i] = { ptr: arena.alloc(bytes, 8), capacity: bytes };
    }
    return state.computedBufs[i].ptr;
  });

  const rowsetPtr = batch.wasm?.rowsetPtr ||
    writer.write(batch.columns, rowCount, false).rowsetPtr;

  const dv = arena.view();
  for (let k = 0; k < computed.length; ++k) {
    writePointer(dv, state.outArrayPtr + k * 8, computedBufs[k]);
  }

  if (computed.length > 0) {
    // memory64 entry: rowset and out-array pointers are i64 params.
    kernel.fn(BigInt(rowsetPtr), BigInt(state.outArrayPtr));
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
      if (col.isString) {
        const values = new Array(rowCount);
        const sv = layout.stringView;
        const memBytes = new Uint8Array(kernel.holder.memory.buffer);
        const decoder = new TextDecoder();
        for (let i = 0; i < rowCount; ++i) {
          const base = bufPtr + i * sv.size;
          const dataPtr = outDv.getUint32(base + sv.data, true);
          const size = Number(outDv.getBigInt64(base + sv.length, true));
          values[i] = decoder.decode(memBytes.subarray(dataPtr, dataPtr + size));
        }
        columns.push({ name: col.name, type: col.type, values });
      } else {
        const values = copyNumericColumnFromMemory(
          kernel.holder.memory, bufPtr, col.type, rowCount);
        columns.push({ name: col.name, type: col.type, values });
      }
    }
  }
  return columns;
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
  attachBatchWasm(batch, rowsetPtr, { pinned: true, selection: null });
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

// Materializes one output batch from a join/cross pair buffer, copies it into
// JS arrays, then frees every wasm buffer the kernel allocated for it (mirrors
// native DestroyKernelOwnedRowSet). Without this the per-drain output would
// accumulate in the shared memory for the whole query — unbounded for joins
// that emit large intermediates (TPC-H Q18/Q21). The returned batch is pure JS
// (no pinned wasm pointer), so downstream re-marshals from its columns.
function drainMaterializedBatch(state, maxRows) {
  const { layout, arena } = state;
  const rowsetPtr = arena.alloc(layout.rowset.size, 8);
  const leftPtr = BigInt(state.leftStore.dataPtr());
  const rightPtr = BigInt(state.rightStore.dataPtr());
  const produced = Number(state.materialize(
    BigInt(state.pairBuffer),
    leftPtr, rightPtr, leftPtr, rightPtr,
    BigInt(state.materializeCursor),
    BigInt(maxRows),
    BigInt(rowsetPtr)));
  if (produced <= 0) {
    arena.free(rowsetPtr);
    throw new Error('join materialize failed');
  }
  state.materializeCursor += produced;
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

function finishAggregateState(state) {
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
  return materializeWasmRowSet(arena, layout, output, rowsetPtr);
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
    materializeCursor: 0,
    semiAntiFinalized: false,
    outerFinalized: false,
    finalized: false,
  };
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
  const rowsetPtr = wasm?.pinned
    ? wasm.rowsetPtr
    : marshalRowSet(
        arena, layout, batch.columns, batch.rowCount, false, selection).rowsetPtr;
  const isLeft = side === 0;
  const semiAnti = isLeftSemiAntiJoin(state.stage);
  let batchIdx = -1;
  let batchPtr = rowsetPtr;
  if (isLeft || !semiAnti) {
    const store = isLeft ? state.leftStore : state.rightStore;
    batchIdx = store.push(batch, rowsetPtr);
    batchPtr = store.dataPtr() + batchIdx * layout.rowset.size;
  }
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

function drainJoinPairs(state, maxRows = 1024) {
  const pairLayout = state.layout.pairBuffer;
  const dv = state.arena.view();
  const count = Number(dv.getBigInt64(state.pairBuffer + pairLayout.count, true));
  if (state.materializeCursor >= count) {
    return [];
  }
  const batch = drainMaterializedBatch(state, maxRows);
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

function stableRowSetPtr(state, rowSet) {
  const { arena, layout } = state;
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  return wasm?.pinned
    ? wasm.rowsetPtr
    : marshalRowSet(
        arena, layout, batch.columns, batch.rowCount, false, selection).rowsetPtr;
}

function updateCrossRightState(state, rowSet) {
  const rowsetPtr = stableRowSetPtr(state, rowSet);
  state.rightStore.push(rowSet.batch, rowsetPtr);
  state.rightRows += rowSet.batch.rowCount;
}

function updateCrossLeftState(state, rowSet) {
  if (state.rightRows === 0) {
    return;
  }
  const rowsetPtr = stableRowSetPtr(state, rowSet);
  const batchIdx = state.leftStore.push(rowSet.batch, rowsetPtr);
  const batchPtr = state.leftStore.dataPtr() + batchIdx * state.layout.rowset.size;
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
}

function drainCrossJoinPairs(state, maxRows = 1024) {
  const pairLayout = state.layout.pairBuffer;
  const dv = state.arena.view();
  const count = Number(dv.getBigInt64(state.pairBuffer + pairLayout.count, true));
  if (state.materializeCursor >= count) {
    return [];
  }
  const batch = drainMaterializedBatch(state, maxRows);
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
      if (selection[row]) ++count;
    }
  }
  return count;
}

function pinnedSortRowSetPtr(arena, layout, rowSet) {
  const { batch } = rowSet;
  const wasm = batch.wasm?.pinned ? batch.wasm : null;
  if (wasm) {
    return wasm.rowsetPtr;
  }
  return marshalRowSet(
    arena, layout, batch.columns, batch.rowCount, false, null).rowsetPtr;
}

function gatherRowIds(rowSets, sortedIds, keep) {
  const columns = outputShapeFromRowSets(rowSets).map(col => ({
    name: col.name,
    type: col.type,
    values: new Array(keep),
  }));
  for (let i = 0; i < keep; ++i) {
    const id = sortedIds[i];
    const batch = rowSets[rowIdBatch(id)].batch;
    const row = rowIdRow(id);
    for (let c = 0; c < columns.length; ++c) {
      columns[c].values[i] = batch.columns[c].values[row];
    }
  }
  return { rowCount: keep, columns };
}

function radixSortRowIds(kernel, layout, rowSets, radixKeys) {
  const arena = kernel.holder.arena;

  const n = selectedRowCount(rowSets);
  const keyCount = radixKeys.length;
  const nullable = !!kernel.nullable;
  const workStride = radixKeys.some(key => key.isString) ? 4 : 1;
  const store = new WasmRowStore(arena, layout);
  const storeBatchIndexes = new Array(rowSets.length);
  const rowsetPtrs = new Array(rowSets.length);

  for (let b = 0; b < rowSets.length; ++b) {
    const rowSet = rowSets[b];
    const rowsetPtr = pinnedSortRowSetPtr(arena, layout, rowSet);
    rowsetPtrs[b] = rowsetPtr;
    storeBatchIndexes[b] = store.push(rowSet.batch, rowsetPtr);
  }

  if (n === 0) {
    return { n, rowIdsPtr: 0, rowsetPtrs };
  }

  // Allocate everything up front (a mid-write grow would detach views).
  const rowIdsPtr = arena.alloc(Math.max(n, 1) * 8, 8);
  const workPtr = arena.alloc(Math.max(n, 1) * workStride * 8, 8);
  const countsPtr = arena.alloc((nullable ? 257 : 256) * 4, 4);
  const descsPtr = arena.alloc(Math.max(keyCount, 1), 1);
  const nullsFirstsPtr = nullable ? arena.alloc(Math.max(keyCount, 1), 1) : 0;

  const dv = arena.view();
  for (let k = 0; k < keyCount; ++k) {
    const key = radixKeys[k];
    dv.setUint8(descsPtr + k, key.desc ? 1 : 0);
    if (nullable) {
      dv.setUint8(nullsFirstsPtr + k, key.nullsFirst ? 1 : 0);
    }
  }

  let out = 0;
  for (let b = 0; b < rowSets.length; ++b) {
    const rowSet = rowSets[b];
    const { batch, selection } = rowSet;
    for (let row = 0; row < batch.rowCount; ++row) {
      if (selection && !selection[row]) continue;
      dv.setBigInt64(rowIdsPtr + out * 8, makeRowId(storeBatchIndexes[b], row), true);
      ++out;
    }
  }

  // memory64: every pointer/count is an i64 param.
  if (nullable) {
    kernel.fn(
      BigInt(store.dataPtr()), BigInt(rowIdsPtr), BigInt(workPtr),
      BigInt(countsPtr), BigInt(n), BigInt(descsPtr), BigInt(nullsFirstsPtr));
  } else {
    kernel.fn(
      BigInt(store.dataPtr()), BigInt(rowIdsPtr), BigInt(workPtr),
      BigInt(countsPtr), BigInt(n), BigInt(descsPtr));
  }

  return { n, rowIdsPtr, rowsetPtrs };
}

function readSortedRowIds(arena, rowIdsPtr, count) {
  const outDv = arena.view();
  const sortedIds = new Array(count);
  for (let i = 0; i < count; ++i) {
    sortedIds[i] = outDv.getBigInt64(rowIdsPtr + i * 8, true);
  }
  return sortedIds;
}

function runRadixSortRowSets(kernel, layout, rowSets, radixKeys, limit) {
  const sorted = radixSortRowIds(kernel, layout, rowSets, radixKeys);
  const keep = (limit != null && limit < sorted.n)
    ? Math.max(Number(limit), 0)
    : sorted.n;
  if (sorted.n === 0 || keep === 0) {
    return emptyBatchForRowSets(rowSets);
  }

  const sortedIds = readSortedRowIds(kernel.holder.arena, sorted.rowIdsPtr, keep);
  return gatherRowIds(rowSets, sortedIds, keep);
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

class SourceTask {
  constructor(stage, readSourceBatches, onProgress) {
    this.stage = stage;
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
    this.rows += next.value.rowCount;
    out.push({ batch: next.value, selection: null });
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
    const selection = runFilter(
      this.kernel,
      this.layout,
      fetched.rowSet.batch,
      this.writer,
      { pinned: outEdge.persistent });
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
    this.writer = new RowSetWriter(shared.arena, layout);
    this.projectState = { computedBufs: [], outArrayPtr: 0 };
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

    let columns;
    if (this.stage.wasm) {
      if (!this.kernel) {
        this.kernel = await instantiateKernel(this.stage.wasm, '<project>', this.shared);
      }
      if (fetched.rowSet.batch.wasm && !fetched.rowSet.batch.wasm.pinned) {
        assertStreamingWasmEdge(this.node.inbound[0]);
      }
      columns = runProject(
        this.kernel,
        this.layout,
        fetched.rowSet.batch,
        this.stage.output,
        this.writer,
        this.projectState);
    } else {
      columns = this.stage.output.map(col => ({
        name: col.name,
        type: col.type,
        values: fetched.rowSet.batch.columns[col.inputIndex].values,
      }));
    }
    const batch = { rowCount: fetched.rowSet.batch.rowCount, columns };
    this.rows += batch.rowCount;
    output.push({ batch, selection: fetched.rowSet.selection });
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
      this.pending = finishAggregateState(this.state);
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
    updateAggregateState(this.state, fetched.rowSet);
    this.rows += fetched.rowSet.batch.rowCount;
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

    const drained = drainJoinPairs(this.state);
    if (drained.length > 0) {
      this.ready.push(...drained);
      return this.execute();
    }

    const progressed = this.pullOneInputBatch();
    if (progressed) {
      this.ready.push(...drainJoinPairs(this.state));
      if (this.ready.length > 0) {
        return this.execute();
      }
      return TaskResult.OK;
    }

    if (this.leftDone && this.rightDone) {
      if (isLeftSemiAntiJoin(this.stage) && !this.state.semiAntiFinalized) {
        finalizeSemiAntiJoinState(this.state);
        this.ready.push(...drainJoinPairs(this.state));
        if (this.ready.length > 0) {
          return this.execute();
        }
      }
      if (isOuterJoin(this.stage) && !this.state.outerFinalized) {
        finalizeOuterJoinState(this.state);
        this.ready.push(...drainJoinPairs(this.state));
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

    const drained = drainCrossJoinPairs(this.state);
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
        this.ready.push(...drainCrossJoinPairs(this.state));
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

function gatherTopSortPicks(stateBatch, tempBatch, arena, pickSrcPtr, pickIdxPtr, count) {
  const shape = stateBatch.columns.length > 0
    ? stateBatch.columns
    : tempBatch.columns;
  const columns = shape.map(col => ({
    name: col.name,
    type: col.type,
    values: new Array(count),
  }));
  const dv = arena.view();
  for (let i = 0; i < count; ++i) {
    const src = dv.getUint8(pickSrcPtr + i);
    const idx = dv.getUint32(pickIdxPtr + i * 4, true);
    const batch = src === 0 ? stateBatch : tempBatch;
    for (let c = 0; c < columns.length; ++c) {
      columns[c].values[i] = batch.columns[c].values[idx];
    }
  }
  return { rowCount: count, columns };
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
      return;
    }

    const n = selectedRowCount([rowSet]);
    if (n === 0) {
      return;
    }

    const arena = this.shared.arena;
    const stateRowSet = marshalRowSet(
      arena,
      this.layout,
      this.stateBatch.columns,
      this.stateBatch.rowCount,
      false,
      null);
    const batchRowSetPtr = pinnedSortRowSetPtr(arena, this.layout, rowSet);
    const pickCapacity = Math.min(
      this.limit, this.stateBatch.rowCount + n);
    const workStride = this.stage.radixKeys.some(key => key.isString) ? 4 : 1;
    const rowIdsPtr = arena.alloc(Math.max(n, 1) * 8, 8);
    const workPtr = arena.alloc(Math.max(n, 1) * workStride * 8, 8);
    const countsPtr = arena.alloc((this.stage.radixNullable ? 257 : 256) * 4, 4);
    const pickSrcPtr = arena.alloc(Math.max(pickCapacity, 1), 1);
    const pickIdxPtr = arena.alloc(Math.max(pickCapacity, 1) * 4, 4);

    const dv = arena.view();
    let outRow = 0;
    const { batch, selection } = rowSet;
    for (let row = 0; row < batch.rowCount; ++row) {
      if (selection && !selection[row]) continue;
      dv.setBigInt64(rowIdsPtr + outRow * 8, makeRowId(0, row), true);
      ++outRow;
    }

    const out = Number(this.kernel.fn(
      BigInt(stateRowSet.rowsetPtr),
      BigInt(batchRowSetPtr),
      BigInt(rowIdsPtr),
      BigInt(workPtr),
      BigInt(countsPtr),
      BigInt(n),
      BigInt(pickSrcPtr),
      BigInt(pickIdxPtr),
      BigInt(this.limit)));
    this.stateBatch = gatherTopSortPicks(
      this.stateBatch, rowSet.batch, arena, pickSrcPtr, pickIdxPtr, out);
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
      radixNullable
        ? 'qdb_radix_sort_indices_composite_nullable'
        : 'qdb_radix_sort_indices_composite',
      this.shared);
    kernel.nullable = radixNullable;
    this.pending = runRadixSortRowSets(
      kernel, this.layout, this.inputs, this.stage.radixKeys, null);
    return this.execute();
  }
}

class SinkTask {
  constructor(limit) {
    this.limit = limit;
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
    return materializeRowSets(this.rowSets, this.limit);
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
      new SourceTask(stage, readSourceBatches, onProgress || null),
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
  const sink = makeNode(new SinkTask(limit), 'materialize');
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

function materializeRowSets(rowSets, limit) {
  const first = rowSets.find(rowSet => rowSet.batch.columns.length > 0);
  const columns = first ? first.batch.columns.map(col => col.name) : [];
  const rows = [];
  const offset = limit ? limit.offset : 0;
  const max = limit ? limit.limit : Infinity;
  let skipped = 0;

  for (const rowSet of rowSets) {
    const { batch, selection } = rowSet;
    for (let i = 0; i < batch.rowCount; ++i) {
      if (selection && !selection[i]) continue;
      if (skipped < offset) {
        ++skipped;
        continue;
      }
      if (rows.length >= max) {
        return { columns, rows };
      }
      rows.push(batch.columns.map(col => formatValue(col.values[i])));
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
