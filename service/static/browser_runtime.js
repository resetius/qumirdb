// Browser execution runtime for simple one-to-one pipelines.
//
// Drives the standalone WASM kernels emitted by qdb_plan_export (bundle.exec):
// marshals a columnar batch into a kernel module's linear memory as
// TColumn[]/TRowSet per the verified 8-byte-pointer layout, runs the kernel, and
// reads results back. See PLAN_BROWSER_EXECUTION.md for the ABI/layout facts.

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

// A bump allocator over one query-wide shared memory. All allocations are
// performed before any writes so a mid-write memory.grow() never detaches a view.
class Arena {
  constructor(memory, base) {
    this.memory = memory;
    this.offset = alignUp(Number(base), 16);
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
  const allocSizes = new Map();
  const alloc = (size) => {
    if (!holder.arena) {
      throw new Error('kernel called qdb_alloc but no allocator is bound');
    }
    const n = Number(size);
    const ptr = holder.arena.alloc(Math.max(n, 1), 8);
    allocSizes.set(ptr, n);
    return ptr;
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
    // malloc family over the query's shared linear memory (bump; free no-op).
    qdb_alloc: (size) => BigInt(alloc(size)),
    qdb_realloc: (ptr, size) => {
      const oldPtr = Number(ptr);
      const newPtr = alloc(size);
      if (oldPtr) {
        const oldSize = allocSizes.get(oldPtr) || 0;
        const mem = new Uint8Array(getMemory().buffer);
        mem.copyWithin(newPtr, oldPtr, oldPtr + Math.min(oldSize, Number(size)));
      }
      return BigInt(newPtr);
    },
    qdb_free: () => {},

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

// Drive the fused aggregate kernel (agg_dispatch / agg_measure_keys /
// agg_finalize in one module — they share the hash table through the module's
// linear memory). Mirrors the native TAggregateProcessor protocol; the arena
// is also the kernel's qdb_alloc backing, so nothing ever overlaps.
export function runAggregate(kernel, layout, batch, selection, stage) {
  const state = createAggregateState(kernel, layout, stage);
  updateAggregateState(state, { batch, selection });
  return finishAggregateState(state);
}

function createAggregateState(kernel, layout, stage) {
  const dispatch = kernel.instance.exports['agg_dispatch'];
  const measure = kernel.instance.exports['agg_measure_keys'];
  const finalize = kernel.instance.exports['agg_finalize'];
  if (!dispatch || !measure || !finalize) {
    throw new Error('aggregate kernel is missing an entry');
  }

  const arena = kernel.holder.arena; // qdb_alloc draws from the shared bump pointer

  const keyCount = Number(stage.keyCount);
  const output = stage.output;
  const aggCount = output.length - keyCount;

  // init(ht, capacity)
  const ht = arena.alloc(layout.hashTable.size, 8);
  new Uint8Array(arena.memory.buffer, ht, layout.hashTable.size).fill(0);
  dispatch(BigInt(ht), 0n, 4n, 0n);

  return {
    kernel,
    layout,
    stage,
    dispatch,
    measure,
    finalize,
    arena,
    ht,
    keyCount,
    output,
    aggCount,
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
    kernel,
    layout,
    dispatch,
    measure,
    finalize,
    arena,
    ht,
    keyCount,
    output,
    aggCount,
  } = state;

  const size = Number(
    arena.view().getBigInt64(ht + layout.hashTable.sizeOffset, true));

  // measure(ht, outputKeyBytes, size) → per-key Data byte counts.
  const keyBytesPtr = arena.alloc(Math.max(keyCount, 1) * 8, 8);
  new Uint8Array(arena.memory.buffer, keyBytesPtr, Math.max(keyCount, 1) * 8).fill(0);
  if (Number(measure(BigInt(ht), BigInt(keyBytesPtr), BigInt(size))) !== size) {
    throw new Error('aggregate measure returned an unexpected row count');
  }

  // Output buffers. Key columns are TColumn structs the kernel fills; agg
  // values land in i64 buffers with optional validity bitmaps.
  const colLayout = layout.column;
  const dv0 = arena.view();
  const keyBytes = [];
  for (let i = 0; i < keyCount; ++i) {
    keyBytes.push(Number(dv0.getBigInt64(keyBytesPtr + i * 8, true)));
  }

  const maskBytes = (size + 7) >> 3;
  const keyCols = [];
  for (let i = 0; i < keyCount; ++i) {
    const isString = output[i].type === 'string';
    keyCols.push({
      colPtr: arena.alloc(colLayout.size, 8),
      dataPtr: arena.alloc(Math.max(keyBytes[i], 1), 8),
      offsetsPtr: isString ? arena.alloc((size + 1) * 8, 8) : 0,
      maskPtr: output[i].nullable ? arena.alloc(Math.max(maskBytes, 1), 8) : 0,
      isString,
    });
  }
  const keyColArrPtr = arena.alloc(Math.max(keyCount, 1) * 8, 8);
  const aggBufPtrs = [];
  const aggMaskPtrs = [];
  for (let i = 0; i < aggCount; ++i) {
    aggBufPtrs.push(arena.alloc(Math.max(size, 1) * 8, 8));
    aggMaskPtrs.push(output[keyCount + i].nullable
      ? arena.alloc(Math.max(maskBytes, 1), 8)
      : 0);
  }
  const aggBufArrPtr = arena.alloc(Math.max(aggCount, 1) * 8, 8);
  const aggMaskArrPtr = arena.alloc(Math.max(aggCount, 1) * 8, 8);

  // All allocations done; take views and wire the pointer tables.
  const dv = arena.view();
  for (let i = 0; i < keyCount; ++i) {
    const col = keyCols[i];
    new Uint8Array(arena.memory.buffer, col.colPtr, colLayout.size).fill(0);
    writePointer(dv, col.colPtr + colLayout.data, col.dataPtr);
    if (col.maskPtr) {
      writePointer(dv, col.colPtr + colLayout.mask, col.maskPtr);
    }
    if (col.isString) {
      writePointer(dv, col.colPtr + colLayout.offsets, col.offsetsPtr);
      dv.setUint8(col.colPtr + colLayout.offsetWidth, 8);
    }
    writePointer(dv, keyColArrPtr + i * 8, col.colPtr);
  }
  for (let i = 0; i < aggCount; ++i) {
    writePointer(dv, aggBufArrPtr + i * 8, aggBufPtrs[i]);
    writePointer(dv, aggMaskArrPtr + i * 8, aggMaskPtrs[i]);
  }

  if (Number(finalize(
      BigInt(ht), BigInt(keyColArrPtr), BigInt(aggBufArrPtr),
      BigInt(aggMaskArrPtr), BigInt(size))) !== size) {
    throw new Error('aggregate finalize returned an unexpected row count');
  }
  dispatch(BigInt(ht), 0n, 0n, 2n); // destroy

  // Materialize output columns into JS values.
  const outDv = arena.view();
  const memBytes = arena.bytes();
  const decoder = new TextDecoder();
  const columns = [];
  for (let i = 0; i < output.length; ++i) {
    const spec = output[i];
    const values = new Array(size);
    const isKey = i < keyCount;
    const valid = (isKey ? keyCols[i].maskPtr : aggMaskPtrs[i - keyCount])
      ? readValidityBits(
          memBytes,
          isKey ? keyCols[i].maskPtr : aggMaskPtrs[i - keyCount],
          size)
      : null;
    if (isKey && keyCols[i].isString) {
      const { dataPtr, offsetsPtr } = keyCols[i];
      for (let r = 0; r < size; ++r) {
        if (valid && !valid[r]) { values[r] = null; continue; }
        const begin = Number(outDv.getBigInt64(offsetsPtr + r * 8, true));
        const end = Number(outDv.getBigInt64(offsetsPtr + (r + 1) * 8, true));
        values[r] = decoder.decode(
          memBytes.subarray(dataPtr + begin, dataPtr + end));
      }
    } else if (isKey) {
      const width = coreTypeWidth(spec.type);
      const { dataPtr } = keyCols[i];
      for (let r = 0; r < size; ++r) {
        values[r] = (valid && !valid[r])
          ? null
          : readNumericValue(outDv, dataPtr + r * width, spec.type);
      }
    } else {
      // Agg buffers are i64 slots; f64 results are stored as raw f64 bits.
      const bufPtr = aggBufPtrs[i - keyCount];
      for (let r = 0; r < size; ++r) {
        values[r] = (valid && !valid[r])
          ? null
          : (spec.type === 'f64'
              ? outDv.getFloat64(bufPtr + r * 8, true)
              : outDv.getBigInt64(bufPtr + r * 8, true));
      }
    }
    columns.push({ name: spec.name, type: spec.type, values });
  }
  return { rowCount: size, columns };
}

function createJoinState(kernel, layout, stage) {
  if (!browserRuntimeSupportsJoin(stage)) {
    throw new Error(`browser join type is not implemented: ${stage.joinType}` +
      (stage.hasResidual ? ' with residual' : ''));
  }
  const exports = kernel.instance.exports;
  const required = [
    'jt_init',
    'jt_process_left',
    'jt_process_right',
    'jt_destroy',
    'pb_destroy',
  ];
  for (const name of required) {
    if (typeof exports[name] !== 'function') {
      throw new Error(`join kernel is missing entry ${name}`);
    }
  }
  const semiAnti = isLeftSemiAntiJoin(stage);
  if (semiAnti && !stage.hasResidual) {
    for (const name of ['jt_insert_key_only', 'jt_finalize_semi_anti']) {
      if (typeof exports[name] !== 'function') {
        throw new Error(`join kernel is missing entry ${name}`);
      }
    }
  }
  if (isResidualSemiAntiJoin(stage) &&
      typeof exports.jt_probe_right_stream !== 'function') {
    throw new Error('join kernel is missing entry jt_probe_right_stream');
  }
  if (isOuterJoin(stage) && typeof exports.jt_finalize_outer !== 'function') {
    throw new Error('join kernel is missing entry jt_finalize_outer');
  }

  const arena = kernel.holder.arena;
  const leftTable = arena.alloc(layout.hashTable.size, 8);
  const rightTable = arena.alloc(layout.hashTable.size, 8);
  const pairBuffer = arena.alloc(layout.pairBuffer.size, 8);
  new Uint8Array(arena.memory.buffer, leftTable, layout.hashTable.size).fill(0);
  new Uint8Array(arena.memory.buffer, rightTable, layout.hashTable.size).fill(0);
  new Uint8Array(arena.memory.buffer, pairBuffer, layout.pairBuffer.size).fill(0);
  const keySize = BigInt(stage.keySize || 0);
  if (!exports.jt_init(BigInt(leftTable), 256n, keySize) ||
      !exports.jt_init(BigInt(rightTable), 256n, keySize)) {
    throw new Error('join hash table initialization failed');
  }

  return {
    kernel,
    layout,
    stage,
    arena,
    leftTable,
    rightTable,
    pairBuffer,
    leftStore: new WasmRowStore(arena, layout),
    rightStore: new WasmRowStore(arena, layout),
    // Selections of stored left batches (parallel to leftStore.batches); the
    // residual semi/anti finalize scan must skip unselected rows.
    leftSelections: [],
    matchedLeftIds: isResidualSemiAntiJoin(stage) ? new Set() : null,
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
  const { arena, layout, kernel } = state;
  const { batch, selection } = rowSet;
  const wasm = wasmRowSetForSelection(batch, selection);
  const rowsetPtr = wasm?.pinned
    ? wasm.rowsetPtr
    : marshalRowSet(
        arena, layout, batch.columns, batch.rowCount, false, selection).rowsetPtr;
  const isLeft = side === 0;
  const semiAnti = isLeftSemiAntiJoin(state.stage);
  const exports = kernel.instance.exports;

  if (semiAnti && !isLeft) {
    const ok = state.stage.hasResidual
      ? exports.jt_probe_right_stream(
          BigInt(state.leftTable),
          BigInt(rowsetPtr),
          -1n,
          BigInt(state.pairBuffer),
          BigInt(state.leftStore.dataPtr()),
          BigInt(state.rightStore.dataPtr()))
      : exports.jt_insert_key_only(
          BigInt(state.rightTable),
          0n,
          BigInt(rowsetPtr),
          0n,
          BigInt(state.pairBuffer));
    if (!ok) {
      throw new Error('join kernel update failed');
    }
    if (!state.stage.hasResidual) {
      clearJoinPairBuffer(state);
    }
    return;
  }

  const store = isLeft ? state.leftStore : state.rightStore;
  const batchIdx = store.push(batch, rowsetPtr);
  if (isLeft) {
    state.leftSelections[batchIdx] = selection || null;
  }
  const ok = isLeft
    ? exports.jt_process_left(
        BigInt(state.leftTable),
        BigInt(state.rightTable),
        BigInt(store.dataPtr() + batchIdx * layout.rowset.size),
        BigInt(batchIdx),
        BigInt(state.pairBuffer),
        BigInt(state.leftStore.dataPtr()),
        BigInt(state.rightStore.dataPtr()))
    : exports.jt_process_right(
        BigInt(state.rightTable),
        BigInt(state.leftTable),
        BigInt(store.dataPtr() + batchIdx * layout.rowset.size),
        BigInt(batchIdx),
        BigInt(state.pairBuffer),
        BigInt(state.leftStore.dataPtr()),
        BigInt(state.rightStore.dataPtr()));
  if (!ok) {
    throw new Error('join kernel update failed');
  }
}

function clearJoinPairBuffer(state) {
  state.arena.view().setBigInt64(
    state.pairBuffer + state.layout.pairBuffer.count, 0n, true);
}

function isLeftSemiAntiJoin(stage) {
  return stage.joinType === 'left_semi' ||
    stage.joinType === 'left_anti' ||
    stage.joinType === 'left-semi' ||
    stage.joinType === 'left-anti';
}

function isLeftAntiJoin(stage) {
  return stage.joinType === 'left_anti' || stage.joinType === 'left-anti';
}

function isOuterJoin(stage) {
  return stage.joinType === 'left' || stage.joinType === 'right';
}

// Semi/anti with a residual predicate: the kernel is compiled as INNER (it
// emits raw pairs); the runtime collects matched left ids and finalizes with
// a JS scan over the stored left rows, mirroring the native executor.
function isResidualSemiAntiJoin(stage) {
  return isLeftSemiAntiJoin(stage) && !!stage.hasResidual;
}

function finalizeSemiAntiJoinState(state) {
  if (state.semiAntiFinalized) {
    return;
  }
  const ok = state.kernel.instance.exports.jt_finalize_semi_anti(
    BigInt(state.leftTable),
    BigInt(state.rightTable),
    BigInt(state.pairBuffer));
  if (!ok) {
    throw new Error('join semi/anti finalize failed');
  }
  state.semiAntiFinalized = true;
}

// Outer join: after both sides are consumed, emit own-side rows that never
// matched, with kNullRowId on the opposite side. For RIGHT the kernel runs
// with the sides flipped, so the drained pairs are swapped back to keep left
// ids in slot 0 (mirrors the native FinalizeOuterJoin).
function finalizeOuterJoinState(state) {
  if (state.outerFinalized) {
    return;
  }
  const right = state.stage.joinType === 'right';
  const own = right ? state.rightTable : state.leftTable;
  const opp = right ? state.leftTable : state.rightTable;
  const ok = state.kernel.instance.exports.jt_finalize_outer(
    BigInt(own), BigInt(opp), BigInt(state.pairBuffer));
  if (!ok) {
    throw new Error('join outer finalize failed');
  }
  state.outerFinalized = true;
  return drainJoinPairs(state, 1024, right);
}

// Residual semi/anti: consume the inner kernel's raw pairs into the
// matched-left-id set (dedup) instead of emitting output.
function collectJoinPairIds(state) {
  const pairLayout = state.layout.pairBuffer;
  const dv = state.arena.view();
  const count = Number(dv.getBigInt64(state.pairBuffer + pairLayout.count, true));
  if (count <= 0) {
    return;
  }
  const dataPtr = Number(dv.getBigInt64(state.pairBuffer + pairLayout.data, true));
  for (let i = 0; i < count; ++i) {
    state.matchedLeftIds.add(dv.getBigInt64(dataPtr + i * 16, true));
  }
  dv.setBigInt64(state.pairBuffer + pairLayout.count, 0n, true);
}

// Residual semi/anti finalize: emit each selected stored left row once —
// SEMI keeps matched, ANTI keeps unmatched (native FinalizeResidualSemiAntiJoin).
function finalizeResidualSemiAntiJoinState(state, maxRows = 1024) {
  const anti = isLeftAntiJoin(state.stage);
  const outputs = [];
  let ids = [];
  const flush = () => {
    if (ids.length > 0) {
      outputs.push(materializeJoinOutput(
        state, ids, new Array(ids.length).fill(-1n)));
      ids = [];
    }
  };
  for (let b = 0; b < state.leftStore.batches.length; ++b) {
    const batch = state.leftStore.batches[b];
    const selection = state.leftSelections[b];
    for (let r = 0; r < batch.rowCount; ++r) {
      if (selection && !selection[r]) {
        continue;
      }
      const matched = state.matchedLeftIds.has(makeRowId(b, r));
      if (matched !== anti) {
        ids.push(makeRowId(b, r));
        if (ids.length >= maxRows) {
          flush();
        }
      }
    }
  }
  flush();
  state.semiAntiFinalized = true;
  return outputs;
}

function drainJoinPairs(state, maxRows = 1024, swapPairs = false) {
  const layout = state.layout;
  const pairLayout = layout.pairBuffer;
  const dv = state.arena.view();
  const count = Number(dv.getBigInt64(state.pairBuffer + pairLayout.count, true));
  if (count <= 0) {
    return [];
  }
  const dataPtr = Number(dv.getBigInt64(state.pairBuffer + pairLayout.data, true));
  const outputs = [];
  let cursor = 0;
  while (cursor < count) {
    const n = Math.min(maxRows, count - cursor);
    const leftIds = new Array(n);
    const rightIds = new Array(n);
    for (let i = 0; i < n; ++i) {
      const pair = cursor + i;
      const a = dv.getBigInt64(dataPtr + pair * 16, true);
      const b = dv.getBigInt64(dataPtr + pair * 16 + 8, true);
      leftIds[i] = swapPairs ? b : a;
      rightIds[i] = swapPairs ? a : b;
    }
    outputs.push(materializeJoinOutput(state, leftIds, rightIds));
    cursor += n;
  }
  dv.setBigInt64(state.pairBuffer + pairLayout.count, 0n, true);
  return outputs;
}

function materializeJoinOutput(state, leftIds, rightIds) {
  const output = state.stage.output || [];
  const leftCount = (state.stage.leftColumns || []).length;
  const semiAnti = isLeftSemiAntiJoin(state.stage);
  const columns = output.map((spec, outIndex) => {
    const fromLeft = semiAnti || outIndex < leftCount;
    const srcIndex = fromLeft ? outIndex : outIndex - leftCount;
    const ids = fromLeft ? leftIds : rightIds;
    return {
      name: spec.name,
      type: spec.type,
      values: gatherJoinColumn(
        fromLeft ? state.leftStore : state.rightStore,
        ids,
        srcIndex,
        spec.type),
    };
  });
  return { rowCount: leftIds.length, columns };
}

function gatherJoinColumn(store, ids, columnIndex, type) {
  if (type === 'string') {
    return ids.map(id => {
      if (id === -1n) return null;
      const batch = store.batch(rowIdBatch(id));
      const value = batch.columns[columnIndex].values[rowIdRow(id)];
      return value === undefined ? null : value;
    });
  }
  // Unmatched rows (outer join) become real nulls, so the output must be a
  // plain array; the typed fast path is only for all-matched gathers.
  let hasNullIds = false;
  for (let i = 0; i < ids.length; ++i) {
    if (ids[i] === -1n) {
      hasNullIds = true;
      break;
    }
  }
  const Ctor = hasNullIds ? null : numericArrayConstructor(type);
  const out = Ctor ? new Ctor(ids.length) : new Array(ids.length);
  for (let i = 0; i < ids.length; ++i) {
    const id = ids[i];
    if (id === -1n) {
      out[i] = null;
      continue;
    }
    const batch = store.batch(rowIdBatch(id));
    const value = batch.columns[columnIndex].values[rowIdRow(id)];
    out[i] = value === undefined ? null : value;
  }
  return out;
}

function finishJoinState(state) {
  if (state.finalized) {
    return;
  }
  state.finalized = true;
  const exports = state.kernel.instance.exports;
  exports.jt_destroy(BigInt(state.leftTable));
  exports.jt_destroy(BigInt(state.rightTable));
  exports.pb_destroy(BigInt(state.pairBuffer));
}

// Drive the radix composite sort kernel. The kernel sorts a row-id permutation
// in place and reads key values directly from the marshaled TRowSet, matching
// the native sort path.
export function runRadixSort(kernel, layout, batch, selection, radixKeys, limit) {
  const arena = kernel.holder.arena;

  const live = [];
  for (let i = 0; i < batch.rowCount; ++i) {
    if (selection && !selection[i]) continue;
    live.push(i);
  }
  const n = live.length;
  const keyCount = radixKeys.length;
  const rowset = marshalRowSet(
    arena, layout, batch.columns, batch.rowCount, false, null);
  const nullable = !!kernel.nullable;
  const workStride = radixKeys.some(key => key.isString) ? 4 : 1;

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

  for (let i = 0; i < n; ++i) {
    dv.setBigInt64(rowIdsPtr + i * 8, makeRowId(0, live[i]), true);
  }

  // memory64: every pointer/count is an i64 param.
  if (nullable) {
    kernel.fn(
      BigInt(rowset.rowsetPtr), BigInt(rowIdsPtr), BigInt(workPtr),
      BigInt(countsPtr), BigInt(n), BigInt(descsPtr), BigInt(nullsFirstsPtr));
  } else {
    kernel.fn(
      BigInt(rowset.rowsetPtr), BigInt(rowIdsPtr), BigInt(workPtr),
      BigInt(countsPtr), BigInt(n), BigInt(descsPtr));
  }

  const outDv = arena.view();
  const order = new Array(n);
  for (let i = 0; i < n; ++i) {
    order[i] = rowIdRow(outDv.getBigInt64(rowIdsPtr + i * 8, true));
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
        this.kernel = await instantiateKernel(this.stage.wasm, 'jt_init', this.shared);
      }
      this.state = createJoinState(this.kernel, this.layout, this.stage);
    }

    const progressed = this.pullOneInputBatch();
    if (progressed) {
      if (isResidualSemiAntiJoin(this.stage)) {
        // Inner-typed kernel: pairs feed the matched-id set, not the output.
        collectJoinPairIds(this.state);
      } else {
        this.ready.push(...drainJoinPairs(this.state));
      }
      if (this.ready.length > 0) {
        return this.execute();
      }
      return TaskResult.OK;
    }

    if (this.leftDone && this.rightDone) {
      if (isLeftSemiAntiJoin(this.stage) && !this.state.semiAntiFinalized) {
        if (isResidualSemiAntiJoin(this.stage)) {
          this.ready.push(...finalizeResidualSemiAntiJoinState(this.state));
        } else {
          finalizeSemiAntiJoinState(this.state);
          this.ready.push(...drainJoinPairs(this.state));
        }
        if (this.ready.length > 0) {
          return this.execute();
        }
      }
      if (isOuterJoin(this.stage) && !this.state.outerFinalized) {
        this.ready.push(...finalizeOuterJoinState(this.state));
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
// right side is one row). Buffers both sides, then emits left-batch × right-row
// chunks; any residual predicate runs as the downstream filter node.
class CrossJoinTask {
  constructor(stage) {
    this.stage = stage;
    this.leftBatches = [];
    this.rightBatches = [];
    this.leftDone = false;
    this.rightDone = false;
    this.emitLeft = 0;
    this.emitRight = 0;
    this.done = false;
    this.rows = 0;
  }

  async execute() {
    if (this.done) return TaskResult.FINISHED;
    const output = this.node.outbound[0].connection;

    if (!this.leftDone || !this.rightDone) {
      const progressed = this.pullOneInputBatch();
      if (progressed) return TaskResult.OK;
      if (!this.leftDone || !this.rightDone) return TaskResult.NEED_DATA;
    }

    const rightRows = this.rightBatches.reduce((n, item) => n + item.batch.rowCount, 0);
    while (this.emitLeft < this.leftBatches.length) {
      if (rightRows === 0) break; // empty right side: empty cross product
      if (!output.canPush()) return TaskResult.BLOCKED_OUTPUT;
      const glued = this.glue(
        this.leftBatches[this.emitLeft], this.rightBatches[this.emitRight]);
      if (glued) {
        output.push({ batch: glued, selection: null });
        this.rows += glued.rowCount;
      }
      this.emitRight += 1;
      if (this.emitRight >= this.rightBatches.length) {
        this.emitRight = 0;
        this.emitLeft += 1;
      }
      if (glued) return TaskResult.OK;
    }

    output.finish();
    this.done = true;
    return TaskResult.FINISHED;
  }

  pullOneInputBatch() {
    for (const [idx, list, doneKey] of [
      [0, this.leftBatches, 'leftDone'],
      [1, this.rightBatches, 'rightDone'],
    ]) {
      if (this[doneKey]) continue;
      const fetched = this.node.inbound[idx].connection.fetch();
      if (fetched.result === FetchResult.OK) {
        list.push(fetched.rowSet);
        return true;
      }
      if (fetched.result === FetchResult.FINISHED) {
        this[doneKey] = true;
        return true;
      }
    }
    return false;
  }

  // One output batch = selected left rows × selected right rows of one
  // left-batch/right-batch pair, columns glued left ++ right.
  glue(leftItem, rightItem) {
    const leftRows = selectedRowIndices(leftItem);
    const rightRows = selectedRowIndices(rightItem);
    const total = leftRows.length * rightRows.length;
    if (total === 0) return null;
    const columns = [];
    for (let c = 0; c < leftItem.batch.columns.length; ++c) {
      const src = leftItem.batch.columns[c];
      const values = new Array(total);
      let k = 0;
      for (const lr of leftRows) {
        const value = src.values[lr];
        for (let j = 0; j < rightRows.length; ++j) values[k++] = value;
      }
      columns.push({ name: src.name, type: src.type, values });
    }
    for (let c = 0; c < rightItem.batch.columns.length; ++c) {
      const src = rightItem.batch.columns[c];
      const values = new Array(total);
      let k = 0;
      for (let i = 0; i < leftRows.length; ++i) {
        for (const rr of rightRows) values[k++] = src.values[rr];
      }
      columns.push({ name: src.name, type: src.type, values });
    }
    return { rowCount: total, columns };
  }
}

function selectedRowIndices(item) {
  const rows = [];
  for (let r = 0; r < item.batch.rowCount; ++r) {
    if (item.selection && !item.selection[r]) continue;
    rows.push(r);
  }
  return rows;
}

class TopSortState {
  constructor(stage) {
    this.limit = Math.max(Number(stage.limit || 0), 0);
    this.keys = sortKeysForStage(stage);
    this.heap = [];
    this.specs = null;
    this.nextSeq = 0;
    this.encoder = new TextEncoder();
  }

  add(rowSet) {
    const { batch, selection } = rowSet;
    if (!this.specs) {
      this.specs = batch.columns.map(col => ({ name: col.name, type: col.type }));
    }
    if (this.limit <= 0) {
      return;
    }
    for (let row = 0; row < batch.rowCount; ++row) {
      if (selection && !selection[row]) {
        continue;
      }
      this.offer(makeTopSortCandidate(
        batch, row, this.keys, this.nextSeq++, this.encoder), batch, row);
    }
  }

  offer(candidate, batch, row) {
    if (this.heap.length < this.limit) {
      this.heap.push(materializeTopSortRecord(candidate, batch, row));
      this.siftUp(this.heap.length - 1);
      return;
    }
    if (compareTopSortRecords(candidate, this.heap[0]) >= 0) {
      return;
    }
    this.heap[0] = materializeTopSortRecord(candidate, batch, row);
    this.siftDown(0);
  }

  finish() {
    const records = [...this.heap].sort(compareTopSortRecords);
    const specs = this.specs || [];
    const columns = specs.map((spec, c) => ({
      name: spec.name,
      type: spec.type,
      values: records.map(record => record.values[c]),
    }));
    return { rowCount: records.length, columns };
  }

  siftUp(index) {
    while (index > 0) {
      const parent = (index - 1) >> 1;
      if (!topSortRecordWorse(this.heap[index], this.heap[parent])) {
        break;
      }
      [this.heap[index], this.heap[parent]] = [this.heap[parent], this.heap[index]];
      index = parent;
    }
  }

  siftDown(index) {
    for (;;) {
      const left = index * 2 + 1;
      const right = left + 1;
      let worst = index;
      if (left < this.heap.length &&
          topSortRecordWorse(this.heap[left], this.heap[worst])) {
        worst = left;
      }
      if (right < this.heap.length &&
          topSortRecordWorse(this.heap[right], this.heap[worst])) {
        worst = right;
      }
      if (worst === index) {
        break;
      }
      [this.heap[index], this.heap[worst]] = [this.heap[worst], this.heap[index]];
      index = worst;
    }
  }
}

function sortKeysForStage(stage) {
  if (Array.isArray(stage.keys)) {
    return stage.keys;
  }
  if (Array.isArray(stage.radixKeys)) {
    return stage.radixKeys.map(key => ({
      index: key.index,
      direction: key.desc ? 'desc' : 'asc',
      nulls: key.nullsFirst ? 'nulls-first' : 'nulls-last',
    }));
  }
  throw new Error('top-sort stage is missing sort keys');
}

function makeTopSortCandidate(batch, row, keys, seq, encoder) {
  const keyValues = keys.map(key => {
    const column = batch.columns[key.index];
    const value = column.values[row];
    if (column.type === 'string') {
      return (value === null || value === undefined)
        ? null
        : encoder.encode(String(value));
    }
    return value;
  });
  return { seq, keyValues, keys };
}

function materializeTopSortRecord(candidate, batch, row) {
  return {
    ...candidate,
    values: batch.columns.map(col => col.values[row]),
  };
}

function compareTopSortRecords(left, right) {
  for (let i = 0; i < left.keys.length; ++i) {
    const key = left.keys[i];
    const columnCompare = left.keyValues[i] instanceof Uint8Array
      ? compareBytes
      : compareScalar;
    const c = compareKey(left.keyValues[i], right.keyValues[i], key, columnCompare);
    if (c !== 0) {
      return c;
    }
  }
  return left.seq - right.seq;
}

function topSortRecordWorse(left, right) {
  return compareTopSortRecords(left, right) > 0;
}

class SortTask {
  constructor(stage, layout, shared) {
    this.stage = stage;
    this.layout = layout;
    this.shared = shared;
    this.topSort = stage.kind === 'top-sort' ? new TopSortState(stage) : null;
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
        this.topSort.add(fetched.rowSet);
      } else {
        this.inputs.push(fetched.rowSet);
      }
      this.rows += fetched.rowSet.batch.rowCount;
      return TaskResult.OK;
    }

    if (this.topSort) {
      this.pending = this.topSort.finish();
    } else {
      const batch = compactRowSets(this.inputs);
      if (this.stage.wasm) {
        const radixNullable = !!this.stage.radixNullable;
        const kernel = await instantiateKernel(
          this.stage.wasm,
          radixNullable
            ? 'qdb_radix_sort_indices_composite_nullable'
            : 'qdb_radix_sort_indices_composite',
          this.shared);
        kernel.nullable = radixNullable;
        this.pending = runRadixSort(
          kernel, this.layout, batch, null, this.stage.radixKeys, null);
      } else {
        this.pending = applySort(batch, null, this.stage.keys, null);
      }
    }
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
    return makeNode(new CrossJoinTask(stage), stage.kind);
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

function compactRowSets(rowSets) {
  if (rowSets.length === 0) {
    return { rowCount: 0, columns: [] };
  }
  if (rowSets.length === 1 && !rowSets[0].selection) {
    return rowSets[0].batch;
  }

  const specs = rowSets[0].batch.columns.map(col => ({
    name: col.name,
    type: col.type,
  }));
  const rows = [];
  for (const rowSet of rowSets) {
    const { batch, selection } = rowSet;
    for (let r = 0; r < batch.rowCount; ++r) {
      if (selection && !selection[r]) continue;
      rows.push(batch.columns.map(col => col.values[r]));
    }
  }

  const columns = specs.map((spec, c) => ({
    ...spec,
    values: rows.map(row => row[c]),
  }));
  return { rowCount: rows.length, columns };
}

// Sort a batch by `keys` (each { index, direction, nulls }), folding any
// pending selection into a compacted, reordered batch. When `limit` is set
// (top-sort), only the first `limit` rows are kept. Returns a new batch with no
// selection; downstream nodes see fully materialized ordered rows.
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
