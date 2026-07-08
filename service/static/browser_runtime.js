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

function copyNumericColumnFromMemory(memory, ptr, type, rowCount) {
  const Ctor = numericArrayConstructor(type);
  if (!Ctor) {
    throw new Error(`unsupported numeric column type: ${type}`);
  }
  return new Ctor(memory.buffer, ptr, rowCount).slice();
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

    for (let c = 0; c < columns.length; ++c) {
      const column = columns[c];
      if (column.type === 'string') {
        this.ensureData(c, Math.max(encoded[c].total, 1), 1);
        this.ensureOffsets(c, (rowCount + 1) * 4);
      } else {
        const width = coreTypeWidth(column.type);
        this.ensureData(c, Math.max(rowCount, 1) * width, 8);
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
    // malloc family over the instance's own linear memory (bump; free no-op).
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

// Compile + instantiate a kernel module, wiring the `env` string runtime. Any
// import we don't implement is reported so the caller can fall back to server
// execution rather than run a partially-linked kernel.
export async function instantiateKernel(base64, entryName) {
  const module = await WebAssembly.compile(base64ToBytes(base64));
  const holder = { memory: null, arena: null };
  const env = createQdbEnv(() => holder.memory, holder);
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
  return { instance, fn, holder };
}

// Marshal `columns` (source column order) into a fresh TRowSet inside `arena`.
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
      if (col.isString) {
        const values = new Array(rowCount);
        const sv = layout.stringView;
        const memBytes = new Uint8Array(kernel.instance.exports.memory.buffer);
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
          kernel.instance.exports.memory, bufPtr, col.type, rowCount);
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
  updateAggregateState(state, batch, selection);
  return finishAggregateState(state);
}

function createAggregateState(kernel, layout, stage) {
  const dispatch = kernel.instance.exports['agg_dispatch'];
  const measure = kernel.instance.exports['agg_measure_keys'];
  const finalize = kernel.instance.exports['agg_finalize'];
  if (!dispatch || !measure || !finalize) {
    throw new Error('aggregate kernel is missing an entry');
  }

  const arena = new Arena(kernel.instance);
  kernel.holder.arena = arena; // qdb_alloc draws from the same bump pointer

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

function updateAggregateState(state, batch, selection) {
  if (state.finished) {
    throw new Error('aggregate state is already finalized');
  }
  // update(ht, batch) — the kernel honors rowset.Selection.
  const { rowsetPtr } = state.inputWriter.write(
    batch.columns, batch.rowCount, false, selection);
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
  const timings = [];

  for (const stage of exec.stages) {
    const started = nowMs();
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
    } else if (stage.kind === 'aggregate') {
      const kernel = await instantiateKernel(stage.wasm, 'agg_dispatch');
      batch = runAggregate(kernel, layout, batch, selection, stage);
      selection = null;
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
    timings.push({
      stage: stage.kind,
      rows: batch ? batch.rowCount : 0,
      elapsedMs: nowMs() - started,
    });
  }

  const started = nowMs();
  const result = materialize(batch, selection, limit);
  timings.push({
    stage: 'materialize',
    rows: result.rows.length,
    elapsedMs: nowMs() - started,
  });
  result.timings = timings;
  return result;
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

class OneToOneConnection {
  constructor(capacity = 1) {
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
  constructor(root) {
    this.root = root;
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
    this.schedule(this.root);
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

function connect(src, dst, connection = new OneToOneConnection(1)) {
  const edge = { src, dst, connection };
  src.outbound.push(edge);
  dst.inbound.push(edge);
  return edge;
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
  constructor(stage, layout) {
    this.stage = stage;
    this.layout = layout;
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
    if (!this.kernel) {
      this.kernel = await instantiateKernel(this.stage.wasm, '<kernel>');
    }
    const selection = runFilter(this.kernel, this.layout, fetched.rowSet.batch);
    this.rows += fetched.rowSet.batch.rowCount;
    output.push({ batch: fetched.rowSet.batch, selection });
    return TaskResult.OK;
  }
}

class ProjectTask {
  constructor(stage, layout) {
    this.stage = stage;
    this.layout = layout;
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
        this.kernel = await instantiateKernel(this.stage.wasm, '<project>');
      }
      columns = runProject(this.kernel, this.layout, fetched.rowSet.batch, this.stage.output);
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
  constructor(stage, layout) {
    this.stage = stage;
    this.layout = layout;
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
          this.kernel = await instantiateKernel(this.stage.wasm, 'agg_dispatch');
        }
        this.state = createAggregateState(this.kernel, this.layout, this.stage);
      }
      this.pending = finishAggregateState(this.state);
      return this.execute();
    }

    if (!this.kernel) {
      this.kernel = await instantiateKernel(this.stage.wasm, 'agg_dispatch');
      this.state = createAggregateState(this.kernel, this.layout, this.stage);
    }
    updateAggregateState(this.state, fetched.rowSet.batch, fetched.rowSet.selection);
    this.rows += fetched.rowSet.batch.rowCount;
    return TaskResult.OK;
  }
}

class SortTask {
  constructor(stage, layout) {
    this.stage = stage;
    this.layout = layout;
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
      this.inputs.push(fetched.rowSet);
      this.rows += fetched.rowSet.batch.rowCount;
      return TaskResult.OK;
    }

    const batch = compactRowSets(this.inputs);
    const rowLimit = this.stage.kind === 'top-sort' ? Number(this.stage.limit) : null;
    if (this.stage.wasm) {
      const kernel = await instantiateKernel(
        this.stage.wasm, 'qdb_radix_sort_indices_composite');
      this.pending = runRadixSort(
        kernel, this.layout, batch, null, this.stage.radixKeys, rowLimit);
    } else {
      this.pending = applySort(batch, null, this.stage.keys, rowLimit);
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

export async function executeBrowserPipelineScheduled(exec, readSourceBatches, options = {}) {
  if (!exec || exec.supported !== true) {
    throw new Error(exec?.reason || 'pipeline is not supported for browser execution');
  }
  if (exec.embedWasm !== true) {
    throw new Error('exec plan was produced without embedded wasm kernels');
  }

  const layout = exec.layout;
  let root = null;
  let previous = null;
  let limit = null;
  const nodes = [];

  for (const stage of exec.stages) {
    let node = null;
    if (stage.kind === 'source') {
      node = makeNode(
        new SourceTask(stage, readSourceBatches, options.onProgress || null),
        stage.kind);
      root = node;
    } else if (stage.kind === 'filter') {
      node = makeNode(new FilterTask(stage, layout), stage.kind);
    } else if (stage.kind === 'project') {
      node = makeNode(new ProjectTask(stage, layout), stage.kind);
    } else if (stage.kind === 'aggregate') {
      node = makeNode(new AggregateTask(stage, layout), stage.kind);
    } else if (stage.kind === 'sort' || stage.kind === 'top-sort') {
      node = makeNode(new SortTask(stage, layout), stage.kind);
    } else if (stage.kind === 'limit') {
      limit = { limit: Number(stage.limit), offset: Number(stage.offset || 0) };
      continue;
    } else {
      throw new Error(`unsupported exec stage: ${stage.kind}`);
    }

    nodes.push(node);
    if (previous) {
      connect(previous, node, new OneToOneConnection(1));
    }
    previous = node;
  }

  if (!root || !previous) {
    throw new Error('exec plan has no runnable stages');
  }

  const sink = makeNode(new SinkTask(limit), 'materialize');
  nodes.push(sink);
  connect(previous, sink, new OneToOneConnection(1));

  const scheduler = new SingleThreadedScheduler(root);
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
