// Small separate file so app.js can check this without loading all of
// browser_runtime.js on the main thread.
let cached = null;

export function detectMemory64Support() {
  if (cached !== null) {
    return cached;
  }
  const errors = [];
  for (const key of ['address', 'index']) {
    try {
      new WebAssembly.Memory({ initial: 1n, [key]: 'i64' });
      cached = true;
      return cached;
    } catch (error) {
      errors.push(`${key}: ${error.message || String(error)}`);
    }
  }
  cached = false;
  return cached;
}
