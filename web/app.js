/* app.js - gander's bundled UI.
 *
 * Everything runs locally: it fetches the file tree, renders previews from
 * /raw/<path> and /api/md, and subscribes to /api/events (Server-Sent Events)
 * for live reload when files change on disk.
 */
"use strict";

const $ = (id) => document.getElementById(id);

const IMG = new Set(["png", "jpg", "jpeg", "gif", "webp", "avif", "bmp", "ico", "svg"]);
const VID = new Set(["mp4", "webm", "ogv", "mov", "m4v", "mkv"]);
const AUD = new Set(["mp3", "ogg", "oga", "wav", "flac", "m4a"]);
const MD = new Set(["md", "markdown"]);
const PDF = new Set(["pdf"]);

const state = {
  files: [],
  expanded: new Set(),
  selected: null,
  filter: "",
};

/* ------------------------------------------------------------- helpers --- */

function ext(path) {
  const i = path.lastIndexOf(".");
  return i < 0 ? "" : path.slice(i + 1).toLowerCase();
}

function esc(s) {
  return s.replace(/[&<>"]/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

function rawUrl(path) {
  const enc = path.split("/").map(encodeURIComponent).join("/");
  return `/raw/${enc}?v=${Date.now()}`;
}

/* ------------------------------------------------------------ the tree --- */

function buildTree(files) {
  const root = { name: "", dirs: new Map(), files: [] };
  for (const f of files) {
    const parts = f.split("/");
    let node = root;
    for (let i = 0; i < parts.length - 1; i++) {
      const d = parts[i];
      if (!node.dirs.has(d)) node.dirs.set(d, { name: d, dirs: new Map(), files: [] });
      node = node.dirs.get(d);
    }
    node.files.push({ name: parts[parts.length - 1], path: f });
  }
  return root;
}

function dirPrefix(path, name) {
  return path ? `${path}/${name}` : name;
}

function matchesFilter(path) {
  return !state.filter || path.toLowerCase().includes(state.filter);
}

function dirHasMatch(node, prefix) {
  if (!state.filter) return true;
  for (const f of node.files)
    if (matchesFilter(dirPrefix(prefix, f.name))) return true;
  for (const [name, child] of node.dirs)
    if (dirHasMatch(child, dirPrefix(prefix, name))) return true;
  return false;
}

function renderNode(node, prefix, container) {
  const dirs = [...node.dirs.entries()].sort((a, b) =>
    a[0].localeCompare(b[0], undefined, { sensitivity: "base" }));
  const files = node.files.slice().sort((a, b) =>
    a.name.localeCompare(b.name, undefined, { sensitivity: "base" }));

  for (const [name, child] of dirs) {
    const path = dirPrefix(prefix, name);
    if (!dirHasMatch(child, prefix)) continue;
    const open = state.expanded.has(path) || (state.filter && dirHasMatch(child, prefix));

    const wrap = document.createElement("div");
    wrap.className = "node";

    const row = document.createElement("div");
    row.className = "row";
    row.innerHTML =
      `<span class="caret">${open ? "▾" : "▸"}</span>` +
      `<span class="icon">📁</span><span class="name">${esc(name)}</span>`;
    row.onclick = () => {
      if (state.expanded.has(path)) state.expanded.delete(path);
      else state.expanded.add(path);
      renderTree();
    };
    wrap.appendChild(row);

    const kids = document.createElement("div");
    kids.className = "children" + (open ? "" : " hidden");
    renderNode(child, path, kids);
    wrap.appendChild(kids);
    container.appendChild(wrap);
  }

  for (const f of files) {
    if (!matchesFilter(f.path)) continue;
    const row = document.createElement("div");
    row.className = "row" + (state.selected === f.path ? " selected" : "");
    row.innerHTML =
      `<span class="caret"></span><span class="icon">${iconFor(f.path)}</span>` +
      `<span class="name">${esc(f.name)}</span>`;
    row.onclick = () => select(f.path);
    container.appendChild(row);
  }
}

function iconFor(path) {
  const e = ext(path);
  if (MD.has(e)) return "📝";
  if (IMG.has(e)) return "🖼";
  if (VID.has(e)) return "🎞";
  if (AUD.has(e)) return "🔊";
  if (PDF.has(e)) return "📕";
  return "📄";
}

function renderTree() {
  const tree = buildTree(state.files);
  const el = $("tree");
  el.textContent = "";
  if (!state.files.length) {
    el.innerHTML = '<div class="row"><span class="name">(no files)</span></div>';
    return;
  }
  renderNode(tree, "", el);
}

/* ---------------------------------------------------------- the preview --- */

function head(path, meta) {
  return (
    `<div class="preview-head"><span class="path">${esc(path)}</span>` +
    `<span class="meta">${meta || ""}</span></div>`
  );
}

function codeHtml(text) {
  const lines = text.replace(/\n$/, "").split("\n");
  return (
    '<pre class="code">' +
    lines.map((l) => `<span class="line">${esc(l) || " "}</span>`).join("\n") +
    "</pre>"
  );
}

function select(path) {
  state.selected = path;
  // make sure the tree reveals the selection
  const parts = path.split("/");
  let acc = "";
  for (let i = 0; i < parts.length - 1; i++) {
    acc = acc ? `${acc}/${parts[i]}` : parts[i];
    state.expanded.add(acc);
  }
  renderTree();
  showPreview(path, true);
}

async function showPreview(path, resetScroll) {
  const pane = $("preview");
  const e = ext(path);
  const keepScroll = resetScroll ? 0 : pane.scrollTop;

  const render = (html, center) => {
    pane.innerHTML = html;
    if (!resetScroll) pane.scrollTop = keepScroll;
  };

  try {
    if (MD.has(e)) {
      const res = await fetch(`/api/md?path=${encodeURIComponent(path)}`);
      if (!res.ok) throw new Error(res.statusText);
      const html = await res.text();
      render(head(path, "markdown") + `<div class="preview-body"><article class="markdown">${html}</article></div>`);
    } else if (IMG.has(e)) {
      render(
        head(path, "image") +
          `<div class="preview-body center"><img class="media" alt="${esc(path)}" src="${rawUrl(path)}"></div>`
      );
    } else if (VID.has(e)) {
      render(
        head(path, "video") +
          `<div class="preview-body center"><video class="media" controls preload="metadata" src="${rawUrl(path)}"></video></div>`
      );
    } else if (AUD.has(e)) {
      render(
        head(path, "audio") +
          `<div class="preview-body center"><audio class="media" controls src="${rawUrl(path)}"></audio></div>`
      );
    } else if (PDF.has(e)) {
      render(
        head(path, "pdf") +
          `<div class="preview-body"><iframe class="media" src="${rawUrl(path)}"></iframe></div>`
      );
    } else {
      const res = await fetch(rawUrl(path));
      if (!res.ok) throw new Error(res.statusText);
      let text = await res.text();
      const MAX = 2 * 1024 * 1024;
      let truncated = false;
      if (text.length > MAX) {
        text = text.slice(0, MAX);
        truncated = true;
      }
      const size = res.headers.get("content-length");
      render(
        head(path, `${size ? Number(size).toLocaleString() + " B" : ""}`) +
          `<div class="preview-body">${codeHtml(text)}` +
          (truncated ? '<div class="truncated">… truncated for preview</div>' : "") +
          `</div>`
      );
    }
  } catch (err) {
    render(head(path) + `<div class="preview-body"><p>Could not preview: ${esc(String(err))}</p></div>`);
  }
}

/* ---------------------------------------------------------------- boot ---- */

async function loadTree() {
  const res = await fetch("/api/tree");
  if (!res.ok) return;
  const data = await res.json();
  state.files = data.files || [];
  $("repo-name").textContent = data.name || "gander";
  document.title = `${data.name || "gander"} · gander`;
  const br = $("repo-branch");
  if (data.is_git && data.branch) {
    br.textContent = data.branch;
    br.hidden = false;
  } else {
    br.hidden = true;
  }
  renderTree();
}

function startEvents() {
  if (!("EventSource" in window)) {
    $("status").textContent = "○ offline";
    $("status").classList.add("off");
    return;
  }
  const es = new EventSource("/api/events");
  es.addEventListener("open", () => {
    $("status").innerHTML = "●&nbsp;live";
    $("status").classList.remove("off");
  });
  es.addEventListener("change", async (ev) => {
    let changed = [];
    try {
      changed = (JSON.parse(ev.data).paths) || [];
    } catch (_) {}
    await loadTree();
    if (state.selected && (changed.includes(state.selected) || changed.length === 0))
      showPreview(state.selected, false);
  });
  es.addEventListener("error", () => {
    $("status").textContent = "○ reconnecting";
    $("status").classList.add("off");
  });
}

$("filter").addEventListener("input", (e) => {
  state.filter = e.target.value.trim().toLowerCase();
  renderTree();
});

loadTree();
startEvents();
