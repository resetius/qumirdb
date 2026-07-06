const zoomState = new WeakMap();

const NODE_R = 46;
const LAYER_GAP = 190;
const ROW_GAP = 128;
const PAD = 90;

export function renderGraph(svg, graph, onSelect) {
  while (svg.firstChild) {
    svg.removeChild(svg.firstChild);
  }

  const nodes = graph?.nodes || [];
  const edges = graph?.edges || [];
  const connections = new Map((graph?.connections || []).map(item => [item.id, item]));
  const width = Math.max(svg.clientWidth || 640, 360);
  const height = Math.max(svg.clientHeight || 260, 220);
  svg.appendChild(arrowMarker());

  if (!nodes.length) {
    svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
    zoomState.delete(svg);
    return;
  }

  const layout = layoutGraph(nodes, edges);
  const content = svgEl('g', { class: 'graph-content' });
  svg.appendChild(content);

  for (const edge of edges) {
    const from = layout.nodes.get(edge.from);
    const to = layout.nodes.get(edge.to);
    if (!from || !to) {
      continue;
    }

    const x1 = from.x + NODE_R;
    const y1 = from.y;
    const x2 = to.x - NODE_R;
    const y2 = to.y;
    const mid = x1 + Math.max((x2 - x1) / 2, 48);
    const path = svgEl('path', {
      d: `M ${x1} ${y1} C ${mid} ${y1}, ${mid} ${y2}, ${x2} ${y2}`,
      class: 'graph-edge',
      'marker-end': 'url(#graph-arrow)'
    });
    path.addEventListener('click', () => onSelect({ type: 'edge', edge }));
    content.appendChild(path);

    const connection = connections.get(edge.connection);
    const edgeText = edgeLabelText(connection, edge);
    const edgeLabel = svgEl('text', {
      x: mid,
      y: (y1 + y2) / 2 - 8,
      class: 'graph-edge-label'
    });
    edgeLabel.textContent = edgeText;
    edgeLabel.addEventListener('click', () => onSelect({ type: 'edge', edge }));
    content.appendChild(edgeLabel);
  }

  for (const item of layout.nodes.values()) {
    const group = svgEl('g', { class: 'graph-node-group' });
    const circle = svgEl('circle', {
      cx: item.x,
      cy: item.y,
      r: NODE_R,
      class: 'graph-node'
    });
    const label = svgEl('text', {
      x: item.x,
      y: item.y + 4,
      class: 'graph-label'
    });
    const text = item.node.label || item.node.operator || item.node.kind;
    label.textContent = shortLabel(text);
    const title = svgEl('title', {});
    title.textContent = item.node.tooltip || detailsText(item.node.details) || text;
    group.appendChild(title);
    group.appendChild(circle);
    group.appendChild(label);
    group.addEventListener('click', () => onSelect({ type: 'node', node: item.node }));
    content.appendChild(group);
  }

  const fitted = fitViewBox(layout.bounds, width, height);
  svg.setAttribute('viewBox', viewBoxString(fitted));
  zoomState.set(svg, {
    viewBox: fitted,
    dragging: false,
    dragPoint: null
  });
  ensureInteractions(svg);
}

function layoutGraph(nodes, edges) {
  const byId = new Map(nodes.map((node, index) => [node.id, { node, index }]));
  const outgoing = new Map(nodes.map(node => [node.id, []]));
  const indegree = new Map(nodes.map(node => [node.id, 0]));

  for (const edge of edges) {
    if (!byId.has(edge.from) || !byId.has(edge.to)) {
      continue;
    }
    outgoing.get(edge.from).push(edge.to);
    indegree.set(edge.to, (indegree.get(edge.to) || 0) + 1);
  }

  const depth = new Map(nodes.map(node => [node.id, 0]));
  const queue = nodes
    .filter(node => (indegree.get(node.id) || 0) === 0)
    .map(node => node.id);
  const seen = new Set(queue);
  for (let head = 0; head < queue.length; ++head) {
    const id = queue[head];
    for (const next of outgoing.get(id) || []) {
      depth.set(next, Math.max(depth.get(next) || 0, (depth.get(id) || 0) + 1));
      indegree.set(next, (indegree.get(next) || 0) - 1);
      if ((indegree.get(next) || 0) === 0 && !seen.has(next)) {
        seen.add(next);
        queue.push(next);
      }
    }
  }

  for (const node of nodes) {
    if (!seen.has(node.id)) {
      queue.push(node.id);
    }
  }

  const layers = new Map();
  for (const id of queue) {
    const layer = depth.get(id) || 0;
    if (!layers.has(layer)) {
      layers.set(layer, []);
    }
    layers.get(layer).push(id);
  }

  const layerKeys = Array.from(layers.keys()).sort((a, b) => a - b);
  const maxRows = Math.max(...layerKeys.map(layer => layers.get(layer).length), 1);
  const worldHeight = Math.max((maxRows - 1) * ROW_GAP, 1);
  const positioned = new Map();

  for (const layer of layerKeys) {
    const ids = layers.get(layer);
    ids.sort((a, b) => (byId.get(a)?.index || 0) - (byId.get(b)?.index || 0));
    const layerHeight = (ids.length - 1) * ROW_GAP;
    const y0 = (worldHeight - layerHeight) / 2;
    ids.forEach((id, row) => {
      positioned.set(id, {
        node: byId.get(id).node,
        x: PAD + layer * LAYER_GAP,
        y: PAD + y0 + row * ROW_GAP
      });
    });
  }

  const maxLayer = Math.max(...layerKeys, 0);
  return {
    nodes: positioned,
    bounds: {
      x: 0,
      y: 0,
      width: PAD * 2 + maxLayer * LAYER_GAP,
      height: PAD * 2 + worldHeight
    }
  };
}

function ensureInteractions(svg) {
  if (svg.dataset.graphInteractions === '1') {
    return;
  }
  svg.dataset.graphInteractions = '1';

  svg.addEventListener('wheel', event => {
    const state = zoomState.get(svg);
    if (!state) {
      return;
    }
    event.preventDefault();
    const point = svgPoint(svg, event.clientX, event.clientY, state.viewBox);
    const delta = clamp(event.deltaY, -80, 80);
    const factor = Math.exp(delta * 0.006);
    state.viewBox = zoomViewBox(state.viewBox, factor, point);
    svg.setAttribute('viewBox', viewBoxString(state.viewBox));
  }, { passive: false });

  svg.addEventListener('pointerdown', event => {
    const state = zoomState.get(svg);
    if (!state || event.button !== 0 || isSelectableGraphTarget(event.target)) {
      return;
    }
    state.dragging = true;
    state.dragPoint = { x: event.clientX, y: event.clientY };
    svg.setPointerCapture(event.pointerId);
  });

  svg.addEventListener('pointermove', event => {
    const state = zoomState.get(svg);
    if (!state?.dragging || !state.dragPoint) {
      return;
    }
    const scaleX = state.viewBox.width / Math.max(svg.clientWidth || 1, 1);
    const scaleY = state.viewBox.height / Math.max(svg.clientHeight || 1, 1);
    const dx = (state.dragPoint.x - event.clientX) * scaleX;
    const dy = (state.dragPoint.y - event.clientY) * scaleY;
    state.viewBox = {
      ...state.viewBox,
      x: state.viewBox.x + dx,
      y: state.viewBox.y + dy
    };
    state.dragPoint = { x: event.clientX, y: event.clientY };
    svg.setAttribute('viewBox', viewBoxString(state.viewBox));
  });

  svg.addEventListener('pointerup', event => {
    const state = zoomState.get(svg);
    if (!state) {
      return;
    }
    state.dragging = false;
    state.dragPoint = null;
    if (svg.hasPointerCapture(event.pointerId)) {
      svg.releasePointerCapture(event.pointerId);
    }
  });
}

function isSelectableGraphTarget(target) {
  if (!(target instanceof Element)) {
    return false;
  }
  return Boolean(target.closest('.graph-node-group, .graph-edge, .graph-edge-label'));
}

function fitViewBox(bounds, width, height) {
  const aspect = width / Math.max(height, 1);
  let viewWidth = bounds.width;
  let viewHeight = bounds.height;
  if (viewWidth / viewHeight > aspect) {
    viewHeight = viewWidth / aspect;
  } else {
    viewWidth = viewHeight * aspect;
  }
  return {
    x: bounds.x + bounds.width / 2 - viewWidth / 2,
    y: bounds.y + bounds.height / 2 - viewHeight / 2,
    width: viewWidth,
    height: viewHeight
  };
}

function zoomViewBox(viewBox, factor, point) {
  const minWidth = 260;
  const nextWidth = clamp(viewBox.width * factor, minWidth, viewBox.width * 6);
  const scale = nextWidth / viewBox.width;
  const nextHeight = viewBox.height * scale;
  return {
    x: point.x - (point.x - viewBox.x) * scale,
    y: point.y - (point.y - viewBox.y) * scale,
    width: nextWidth,
    height: nextHeight
  };
}

function svgPoint(svg, clientX, clientY, viewBox) {
  const rect = svg.getBoundingClientRect();
  return {
    x: viewBox.x + ((clientX - rect.left) / Math.max(rect.width, 1)) * viewBox.width,
    y: viewBox.y + ((clientY - rect.top) / Math.max(rect.height, 1)) * viewBox.height
  };
}

function edgeLabelText(connection, edge) {
  const text = connection?.kind || edge.connection;
  return edge.edgeCount && edge.edgeCount > 1 ? `${text} x${edge.edgeCount}` : text;
}

function viewBoxString(viewBox) {
  return `${viewBox.x} ${viewBox.y} ${viewBox.width} ${viewBox.height}`;
}

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function arrowMarker() {
  const defs = svgEl('defs', {});
  const marker = svgEl('marker', {
    id: 'graph-arrow',
    viewBox: '0 0 10 10',
    refX: 9,
    refY: 5,
    markerWidth: 7,
    markerHeight: 7,
    orient: 'auto-start-reverse'
  });
  marker.appendChild(svgEl('path', {
    d: 'M 0 0 L 10 5 L 0 10 z',
    class: 'graph-arrow'
  }));
  defs.appendChild(marker);
  return defs;
}

function detailsText(details) {
  if (!details || typeof details !== 'object') {
    return '';
  }
  return Object.entries(details)
    .map(([key, value]) => `${key}: ${formatValue(value)}`)
    .join('\n');
}

function formatValue(value) {
  if (Array.isArray(value)) {
    return value.map(item => formatValue(item)).join(', ');
  }
  if (value && typeof value === 'object') {
    return Object.entries(value)
      .map(([key, item]) => `${key}=${formatValue(item)}`)
      .join(' ');
  }
  return String(value ?? '');
}

function shortLabel(value) {
  const text = String(value || '');
  return text.length > 22 ? `${text.slice(0, 19)}...` : text;
}

function svgEl(name, attrs) {
  const el = document.createElementNS('http://www.w3.org/2000/svg', name);
  for (const [key, value] of Object.entries(attrs)) {
    el.setAttribute(key, String(value));
  }
  return el;
}
