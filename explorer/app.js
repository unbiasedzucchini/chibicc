// chibicc Explorer - app.js

const EXAMPLES = {
  hello: `#include <stdio.h>

int main() {
  printf("Hello, world!\\n");
  return 0;
}`,
  fibonacci: `int fibonacci(int n) {
  if (n <= 1) return n;
  return fibonacci(n - 1) + fibonacci(n - 2);
}

int main() {
  return fibonacci(10);
}`,
  structs: `struct Point {
  int x;
  int y;
};

int distance_sq(struct Point a, struct Point b) {
  int dx = a.x - b.x;
  int dy = a.y - b.y;
  return dx * dx + dy * dy;
}

int main() {
  struct Point p1 = {3, 4};
  struct Point p2 = {0, 0};
  return distance_sq(p1, p2);
}`,
  pointers: `int swap(int *a, int *b) {
  int tmp = *a;
  *a = *b;
  *b = tmp;
  return 0;
}

int main() {
  int x = 10;
  int y = 20;
  swap(&x, &y);
  return x;
}`,
  loops: `int main() {
  int arr[10];
  int i;
  for (i = 0; i < 10; i++)
    arr[i] = i * i;

  int sum = 0;
  i = 0;
  while (i < 10) {
    sum = sum + arr[i];
    i++;
  }
  return sum;
}`
};

const STAGES = [
  { id: 'tokenize',    name: 'Tokenize',    desc: 'Source text → token stream' },
  { id: 'preprocess',  name: 'Preprocess',  desc: 'Macro expansion & includes' },
  { id: 'parse',       name: 'Parse',        desc: 'Tokens → abstract syntax tree' },
  { id: 'codegen',     name: 'Codegen',      desc: 'AST → x86-64 assembly' },
];

let editor;
let lastResult = null;
let activeStage = null;

// ── Init ──

function init() {
  editor = CodeMirror.fromTextArea(document.getElementById('source-editor'), {
    mode: 'text/x-csrc',
    lineNumbers: true,
    tabSize: 2,
    indentWithTabs: false,
    matchBrackets: true,
  });

  document.getElementById('compile-btn').addEventListener('click', compile);
  document.getElementById('example-select').addEventListener('change', (e) => {
    if (e.target.value && EXAMPLES[e.target.value]) {
      editor.setValue(EXAMPLES[e.target.value]);
      e.target.value = '';
      compile();
    }
  });

  // Ctrl+Enter to compile
  editor.setOption('extraKeys', {
    'Ctrl-Enter': compile,
    'Cmd-Enter': compile,
  });

  // Resizable source panel
  initResize();

  // Auto-compile on load
  compile();
}

// ── Resize ──

function initResize() {
  const panel = document.getElementById('source-panel');
  let dragging = false;

  panel.addEventListener('mousedown', (e) => {
    const rect = panel.getBoundingClientRect();
    if (e.clientX >= rect.right - 6) {
      dragging = true;
      document.body.style.cursor = 'col-resize';
      document.body.style.userSelect = 'none';
      e.preventDefault();
    }
  });

  document.addEventListener('mousemove', (e) => {
    if (!dragging) return;
    const pct = (e.clientX / window.innerWidth) * 100;
    panel.style.width = Math.max(15, Math.min(75, pct)) + '%';
    editor.refresh();
  });

  document.addEventListener('mouseup', () => {
    if (dragging) {
      dragging = false;
      document.body.style.cursor = '';
      document.body.style.userSelect = '';
    }
  });
}

// ── Compile ──

async function compile() {
  const btn = document.getElementById('compile-btn');
  const status = document.getElementById('status');
  btn.classList.add('compiling');
  btn.textContent = 'Compiling…';
  status.textContent = '';
  clearError();

  const code = editor.getValue();

  try {
    const start = performance.now();
    const resp = await fetch('/api/compile', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code }),
    });

    if (!resp.ok) {
      const text = await resp.text();
      throw new Error(text);
    }

    lastResult = await resp.json();
    const elapsed = performance.now() - start;
    status.textContent = `${elapsed.toFixed(0)}ms`;

    if (lastResult.error) {
      showError(lastResult.error);
    }

    renderPipeline();

    // Re-render active detail if open
    if (activeStage) {
      renderDetail(activeStage);
    }

  } catch (err) {
    showError(err.message);
  } finally {
    btn.classList.remove('compiling');
    btn.textContent = 'Compile';
  }
}

// ── Error display ──

function showError(msg) {
  let bar = document.querySelector('.error-bar');
  if (!bar) {
    bar = document.createElement('div');
    bar.className = 'error-bar';
    document.body.appendChild(bar);
  }
  bar.textContent = msg;
}

function clearError() {
  const bar = document.querySelector('.error-bar');
  if (bar) bar.remove();
}

// ── Pipeline rendering ──

function renderPipeline() {
  const el = document.getElementById('pipeline');
  if (!lastResult) { el.innerHTML = ''; return; }

  const sizes = STAGES.map(s => getStageSizeValue(s.id));
  const maxSize = Math.max(...sizes, 1);

  el.innerHTML = STAGES.map((stage, i) => {
    const stats = lastResult.stages[stage.id];
    const isActive = activeStage === stage.id;
    const sizeVal = sizes[i];
    const barPct = (sizeVal / maxSize) * 100;

    return `
      ${i > 0 ? '<div class="stage-arrow">▼</div>' : ''}
      <div class="stage ${isActive ? 'active' : ''}" data-stage="${stage.id}">
        <div class="stage-index">${i + 1}</div>
        <div class="stage-info">
          <div class="stage-name">${stage.name}</div>
          <div class="stage-meta">${stage.desc}${stats ? ' · ' + stats.time_ms.toFixed(1) + 'ms' : ''}</div>
        </div>
        <div class="stage-bar-wrap">
          <div class="stage-bar"><div class="stage-bar-fill" style="width:${barPct}%"></div></div>
          <div class="stage-size">${getSizeLabel(stage.id)}</div>
        </div>
      </div>
    `;
  }).join('');

  el.querySelectorAll('.stage').forEach(el => {
    el.addEventListener('click', () => {
      const id = el.dataset.stage;
      if (activeStage === id) {
        activeStage = null;
        document.getElementById('detail-view').classList.add('hidden');
        el.classList.remove('active');
      } else {
        activeStage = id;
        renderDetail(id);
        renderPipeline(); // update active states
      }
    });
  });
}

function getStageSizeValue(id) {
  if (!lastResult || !lastResult.stages[id]) return 0;
  const s = lastResult.stages[id];
  switch (id) {
    case 'tokenize':   return s.count || 0;
    case 'preprocess': return s.lines || 0;
    case 'parse':      return s.nodes || 0;
    case 'codegen':    return s.bytes || 0;
  }
  return 0;
}

function getSizeLabel(id) {
  if (!lastResult || !lastResult.stages[id]) return '';
  const s = lastResult.stages[id];
  switch (id) {
    case 'tokenize':   return `${s.count || 0} tokens`;
    case 'preprocess': return `${s.lines || 0} lines`;
    case 'parse': {
      const parts = [];
      if (s.functions) parts.push(`${s.functions} fn${s.functions > 1 ? 's' : ''}`);
      if (s.nodes) parts.push(`${s.nodes} nodes`);
      return parts.join(', ') || '0 nodes';
    }
    case 'codegen': {
      if (s.bytes >= 1024) return (s.bytes / 1024).toFixed(1) + ' KB';
      return `${s.bytes || 0} B`;
    }
  }
  return '';
}

// ── Detail rendering ──

function renderDetail(stageId) {
  const dv = document.getElementById('detail-view');
  dv.classList.remove('hidden');

  const stage = STAGES.find(s => s.id === stageId);
  let contentHTML = '';

  switch (stageId) {
    case 'tokenize':   contentHTML = renderTokens(); break;
    case 'preprocess': contentHTML = renderPreprocessed(); break;
    case 'parse':      contentHTML = renderAST(); break;
    case 'codegen':    contentHTML = renderAssembly(); break;
  }

  dv.innerHTML = `
    <div class="detail-header">
      <div class="detail-title">${stage.name}</div>
      <button class="detail-close" id="detail-close">×</button>
    </div>
    ${contentHTML}
  `;

  document.getElementById('detail-close').addEventListener('click', () => {
    activeStage = null;
    dv.classList.add('hidden');
    renderPipeline();
  });
}

function renderTokens() {
  if (!lastResult || !lastResult.tokens || lastResult.tokens === null) {
    return '<div class="detail-content">No token data</div>';
  }

  const tokens = lastResult.tokens;
  if (!Array.isArray(tokens) || tokens.length === 0) {
    return '<div class="detail-content">No tokens</div>';
  }

  const kindColors = {
    TK_KEYWORD: '#8a60a0',
    TK_IDENT: '#5a7aaa',
    TK_PUNCT: '#3a3530',
    TK_NUM: '#b07040',
    TK_STR: '#6a8a50',
    TK_PP_NUM: '#b07040',
  };

  const rows = tokens.slice(0, 500).map(t => {
    const kind = t.kind.replace('TK_', '');
    const color = kindColors[t.kind] || '#3a3530';
    const text = escapeHTML(t.text || '');
    return `<div class="token-row">
      <span class="token-type" style="color:${color}">${kind}</span>
      <span>${text}</span>
      <span class="token-pos">${t.line || ''}</span>
    </div>`;
  }).join('');

  const overflow = tokens.length > 500 ? `<div style="padding:8px 10px;color:var(--text-dim);font-size:11px">…and ${tokens.length - 500} more tokens</div>` : '';

  return `<div class="token-grid">
    <div class="token-header"><span>Kind</span><span>Text</span><span>Line</span></div>
    ${rows}
    ${overflow}
  </div>`;
}

function renderPreprocessed() {
  if (!lastResult || !lastResult.preprocessed) {
    return '<div class="detail-content">No preprocessed output</div>';
  }
  return `<div class="detail-content">${escapeHTML(lastResult.preprocessed)}</div>`;
}

function renderAST() {
  if (!lastResult || !lastResult.ast || lastResult.ast === null) {
    return '<div class="detail-content">No AST data</div>';
  }

  const ast = lastResult.ast;
  const html = renderASTNode(ast, 0);
  return `<div class="detail-content">${html}</div>`;
}

function renderASTNode(obj, depth) {
  if (depth > 25) return '<span class="ast-collapsed-hint">…</span>';

  if (obj === null || obj === undefined) return '<span class="ast-null">null</span>';
  if (typeof obj === 'boolean') return `<span class="ast-boolean">${obj}</span>`;
  if (typeof obj === 'number') return `<span class="ast-number">${obj}</span>`;
  if (typeof obj === 'string') return `<span class="ast-string">"${escapeHTML(obj)}"</span>`;

  if (Array.isArray(obj)) {
    if (obj.length === 0) return '<span class="ast-bracket">[]</span>';
    const id = 'ast-' + Math.random().toString(36).slice(2, 8);
    const items = obj.map(item => {
      const indent = '  '.repeat(depth + 1);
      return indent + renderASTNode(item, depth + 1);
    }).join(',\n');
    const hint = `${obj.length} items`;
    return `<span class="ast-toggle" data-target="${id}" data-collapsed="${id}-hint" onclick="toggleAST(this)">[▼</span><span id="${id}">\n${items}\n${'  '.repeat(depth)}</span><span id="${id}-hint" class="hidden ast-collapsed-hint">${hint}</span><span class="ast-bracket">]</span>`;
  }

  if (typeof obj === 'object') {
    const keys = Object.keys(obj);
    if (keys.length === 0) return '<span class="ast-bracket">{}</span>';

    // Show kind prominently if present
    const kindVal = obj.kind;
    const id = 'ast-' + Math.random().toString(36).slice(2, 8);
    const entries = keys.map(k => {
      const indent = '  '.repeat(depth + 1);
      const keyClass = k === 'kind' ? 'ast-type' : 'ast-key';
      return `${indent}<span class="${keyClass}">"${escapeHTML(k)}"</span>: ${renderASTNode(obj[k], depth + 1)}`;
    }).join(',\n');

    const hint = kindVal ? kindVal : `${keys.length} keys`;
    return `<span class="ast-toggle" data-target="${id}" data-collapsed="${id}-hint" onclick="toggleAST(this)">{\u25bc</span><span id="${id}">\n${entries}\n${'  '.repeat(depth)}</span><span id="${id}-hint" class="hidden ast-collapsed-hint">${hint}</span><span class="ast-bracket">}</span>`;
  }

  return escapeHTML(String(obj));
}

window.toggleAST = function(el) {
  const target = document.getElementById(el.dataset.target);
  const hint = document.getElementById(el.dataset.collapsed);
  if (!target) return;
  const isCollapsed = target.classList.contains('hidden');
  target.classList.toggle('hidden');
  hint.classList.toggle('hidden');
  // Update arrow
  const text = el.textContent;
  if (text.includes('▼')) {
    el.textContent = text.replace('▼', '▶');
  } else {
    el.textContent = text.replace('▶', '▼');
  }
};

function renderAssembly() {
  if (!lastResult || !lastResult.assembly) {
    return '<div class="detail-content">No assembly output</div>';
  }

  const lines = lastResult.assembly.split('\n');
  const highlighted = lines.map(line => {
    // Label lines
    if (/^\S+:/.test(line)) {
      return `<span style="color:#a07020;font-weight:600">${escapeHTML(line)}</span>`;
    }
    // Directive lines
    if (/^\s*\./.test(line)) {
      return `<span style="color:#8a60a0">${escapeHTML(line)}</span>`;
    }
    // Comment lines
    if (/^\s*#/.test(line)) {
      return `<span style="color:#a09888">${escapeHTML(line)}</span>`;
    }
    // Instruction lines - highlight opcode
    const m = line.match(/^(\s+)(\S+)(.*)?$/);
    if (m) {
      return `${m[1]}<span style="color:#5a7aaa">${escapeHTML(m[2])}</span>${escapeHTML(m[3] || '')}`;
    }
    return escapeHTML(line);
  }).join('\n');

  return `<div class="detail-content">${highlighted}</div>`;
}

function escapeHTML(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

// ── Boot ──
document.addEventListener('DOMContentLoaded', init);
