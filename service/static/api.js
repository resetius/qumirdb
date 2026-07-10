export async function getJson(path) {
  const response = await fetch(path);
  const text = await response.text();
  let data;
  try {
    data = JSON.parse(text);
  } catch {
    data = { ok: false, error: { message: text || response.statusText } };
  }
  if (!response.ok && data.ok !== false) {
    data = { ok: false, error: { message: response.statusText } };
  }
  return data;
}

export async function postJson(path, payload, signal) {
  const response = await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
    signal
  });
  const text = await response.text();
  let data;
  try {
    data = JSON.parse(text);
  } catch {
    data = { ok: false, error: { message: text || response.statusText } };
  }
  if (!response.ok && data.ok !== false) {
    data = { ok: false, error: { message: response.statusText } };
  }
  return data;
}
