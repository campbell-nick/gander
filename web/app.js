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

/* How each git status is shown: a one-letter badge and a colour class. */
const STATUS = {
  modified: { letter: "M", cls: "st-mod" },
  added: { letter: "A", cls: "st-add" },
  deleted: { letter: "D", cls: "st-del" },
  renamed: { letter: "R", cls: "st-ren" },
  copied: { letter: "C", cls: "st-ren" },
  untracked: { letter: "U", cls: "st-unt" },
  conflicted: { letter: "U", cls: "st-con" },
  typechange: { letter: "T", cls: "st-mod" },
};

const state = {
  files: [],
  expanded: new Set(),
  selected: null,
  filter: "",
  meta: null,
  isGit: false,
  status: new Map(), // path -> { path, code, status }
  log: [],
  view: "preview", // "preview" | "git"
  mode: "content", // "content" | "diff" (what the preview pane shows)
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

function formatBytes(n) {
  if (!n) return "0 B";
  const u = ["B", "KiB", "MiB", "GiB"];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) {
    n /= 1024;
    i++;
  }
  return `${n < 10 && i > 0 ? n.toFixed(1) : Math.round(n)} ${u[i]}`;
}

/* -------------------------------------------------------------- git state --- */

/* Status of a file: its own entry, or the enclosing untracked directory. */
function statusFor(path) {
  const e = state.status.get(path);
  if (e) return e.status;
  let idx = path.lastIndexOf("/");
  while (idx > 0) {
    const key = path.slice(0, idx) + "/";
    const d = state.status.get(key);
    if (d) return d.status;
    idx = path.lastIndexOf("/", idx - 1);
  }
  return null;
}

/* Every directory that contains a change, for a marker on folder rows. */
function dirtyDirs() {
  const s = new Set();
  for (const p of state.status.keys()) {
    let idx = p.lastIndexOf("/");
    while (idx > 0) {
      s.add(p.slice(0, idx));
      idx = p.lastIndexOf("/", idx - 1);
    }
  }
  return s;
}

function statusBadge(status) {
  const s = STATUS[status];
  if (!s) return "";
  return `<span class="st ${s.cls}" title="${status}">${s.letter}</span>`;
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

function renderNode(node, prefix, container, dirty) {
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
    const mark = dirty.has(path) ? '<span class="st-dot" title="contains changes"></span>' : "";
    row.innerHTML =
      `<span class="caret">${open ? "▾" : "▸"}</span>` +
      `<span class="icon">📁</span><span class="name">${esc(name)}</span>${mark}`;
    row.onclick = () => {
      if (state.expanded.has(path)) state.expanded.delete(path);
      else state.expanded.add(path);
      renderTree();
    };
    wrap.appendChild(row);

    const kids = document.createElement("div");
    kids.className = "children" + (open ? "" : " hidden");
    renderNode(child, path, kids, dirty);
    wrap.appendChild(kids);
    container.appendChild(wrap);
  }

  for (const f of files) {
    if (!matchesFilter(f.path)) continue;
    const st = statusFor(f.path);
    const row = document.createElement("div");
    row.className = "row" + (state.selected === f.path ? " selected" : "");
    row.innerHTML =
      `<span class="caret"></span><span class="icon">${iconFor(f.path)}</span>` +
      `<span class="name">${esc(f.name)}</span>` +
      (st ? statusBadge(st) : "");
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
  const sidebar = document.querySelector(".sidebar");
  const sc = sidebar ? sidebar.scrollTop : 0;
  const tree = buildTree(state.files);
  const el = $("tree");
  el.textContent = "";
  if (!state.files.length) {
    el.innerHTML = '<div class="row"><span class="name">(no files)</span></div>';
    return;
  }
  renderNode(tree, "", el, dirtyDirs());
  if (sidebar) sidebar.scrollTop = sc;
}

/* ---------------------------------------------------------- the preview --- */

function head(path, meta, actions) {
  return (
    `<div class="preview-head"><span class="path">${esc(path)}</span>` +
    (actions ? `<span class="head-actions">${actions}</span>` : "") +
    `<span class="meta">${meta || ""}</span></div>`
  );
}

/* Whether the repo has CI workflows at all, read straight from disk. gander
 * never touches the network, so it shows that CI is configured - not a result
 * it would have to invent. */
function ciChip(m) {
  if (!m.ci || !m.ci.length) return `<span class="chip">no CI configured</span>`;
  const detail = (m.ci || []).join(", ");
  return `<span class="chip accent" title="${esc(detail)}">CI configured</span>`;
}

/* GitHub-style repository home card, built entirely from local data. */
function renderHome(resetScroll = true) {
  const pane = $("preview");
  const keep = resetScroll ? 0 : pane.scrollTop;
  const m = state.meta;
  if (!m) {
    pane.innerHTML =
      '<div class="welcome"><h1>gander</h1>' +
      "<p>Select a file to preview it. Markdown, images, GIFs, video and code " +
      "are all rendered locally — nothing leaves your machine.</p></div>";
    pane.scrollTop = keep;
    return;
  }

  const name = m.name || "gander";
  const chips = [];
  if (m.is_git && m.branch)
    chips.push(`<span class="chip branch">${esc(m.branch)}</span>`);
  if (m.latest_tag)
    chips.push(`<span class="chip">🏷 <b>${esc(m.latest_tag)}</b>${
      m.tag_count > 1 ? ` <span>+${m.tag_count - 1}</span>` : ""}</span>`);
  chips.push(ciChip(m));
  if (m.license) chips.push(`<span class="chip">⚖ ${esc(m.license)}</span>`);
  if (m.is_git) {
    chips.push(`<span class="chip">${m.commits.toLocaleString()} commits</span>`);
    chips.push(
      `<span class="chip clickable" data-action="git" title="show changes">${m.dirty
        ? `<span class="dot" style="color:var(--yellow)"></span>${m.changes} uncommitted`
        : `<span class="dot" style="color:var(--green)"></span>clean`}</span>`
    );
  }
  chips.push(`<span class="chip">${m.files.toLocaleString()} files · ${formatBytes(m.total_bytes)}</span>`);

  /* language bar */
  const langs = (m.languages || []).filter((l) => l.bytes > 0);
  const total = langs.reduce((a, l) => a + l.bytes, 0) || 1;
  let bar = "", legend = "";
  for (const l of langs) {
    const pct = (l.bytes / total) * 100;
    bar += `<span style="width:${pct.toFixed(2)}%;background:${l.color}" title="${esc(l.name)} ${pct.toFixed(1)}%"></span>`;
    legend += `<span><i style="background:${l.color}"></i>${esc(l.name)} <b>${pct.toFixed(1)}%</b></span>`;
  }

  let last = "";
  if (m.is_git && m.last && m.last.hash) {
    last =
      '<div class="lastcommit">' +
      `<span class="hash">${esc(m.last.hash)}</span>` +
      `<span class="subject">${esc(m.last.subject)}</span>` +
      `<span class="who">${esc(m.last.author)} · ${esc(m.last.when)}</span>` +
      "</div>";
  }

  const desc = m.description
    ? `<p class="repo-desc">${esc(m.description)}</p>`
    : "";

  pane.innerHTML =
    `<div class="preview-head"><span class="path">${esc(name)}</span>` +
    `<span class="meta">git repository</span></div>` +
    '<div class="preview-body"><div class="repo-home">' +
    `<h1 class="repo-h1">${esc(name)}</h1>` +
    desc +
    `<div class="chips">${chips.join("")}</div>` +
    (bar ? `<div class="langbar">${bar}</div><div class="langlegend">${legend}</div>` : "") +
    last +
    '<p class="hint">Select a file from the tree to preview it. Edits on disk appear here automatically.</p>' +
    "</div>" +
    (m.readme ? '<section class="readme-card" id="home-readme"></section>' : "") +
    "</div>";
  if (m.readme) loadHomeReadme(m.readme, keep);
  pane.scrollTop = keep;
}

/* Render the README below the home card, the way a repo landing page does.
 * keep is the scroll offset to restore once the README is in place, so a
 * refresh (e.g. after a commit) does not jump to the top. */
let homeReadmeToken = 0;
async function loadHomeReadme(path, keep = 0) {
  const token = ++homeReadmeToken;
  let html;
  try {
    const res = await fetch(`/api/md?path=${encodeURIComponent(path)}`);
    if (!res.ok) return;
    html = await res.text();
  } catch (_) {
    return;
  }
  if (token !== homeReadmeToken) return;
  const el = document.getElementById("home-readme");
  if (!el) return;
  el.innerHTML =
    `<div class="readme-head">${esc(path)}</div>` +
    `<article class="markdown">${html}</article>`;
  const pane = $("preview");
  if (pane) pane.scrollTop = keep;
}

function showHome() {
  state.view = "preview";
  state.selected = null;
  renderTree();
  renderHome();
}

/* The git panel: working-tree changes grouped by state, plus recent commits. */
function renderGit() {
  const pane = $("preview");
  state.view = "git";
  const m = state.meta || {};

  const groups = { conflicted: [], staged: [], changes: [], untracked: [] };
  for (const [path, e] of state.status) {
    if (e.code === "??") groups.untracked.push(path);
    else if (e.status === "conflicted") groups.conflicted.push(path);
    else {
      if (e.code[0] !== " " && e.code[0] !== "?") groups.staged.push(path);
      if (e.code[1] !== " " && e.code[1] !== "?") groups.changes.push(path);
    }
  }
  for (const k of Object.keys(groups))
    groups[k].sort((a, b) => a.localeCompare(b));

  const total = state.status.size;

  const group = (title, paths) => {
    if (!paths.length) return "";
    const rows = paths
      .map((p) => {
        const st = statusFor(p) || "modified";
        return (
          `<li class="change" data-action="select" data-path="${esc(p)}">` +
          statusBadge(st) +
          `<span class="cpath">${esc(p)}</span></li>`
        );
      })
      .join("");
    return (
      `<div class="change-group"><h3>${title} <span class="count">${paths.length}</span></h3>` +
      `<ul class="changelist">${rows}</ul></div>`
    );
  };

  const changesHtml =
    total === 0
      ? '<p class="clean-note">Working tree clean — nothing to commit.</p>'
      : group("Conflicts", groups.conflicted) +
        group("Staged", groups.staged) +
        group("Changes", groups.changes) +
        group("Untracked", groups.untracked);

  const commits = state.log.length
    ? state.log
        .map(
          (c) =>
            `<li class="commit"><span class="hash">${esc(c.hash)}</span>` +
            `<span class="subject">${esc(c.subject)}</span>` +
            `<span class="who">${esc(c.author)} · ${esc(c.when)}</span></li>`
        )
        .join("")
    : '<li class="commit empty">No commits yet.</li>';

  const branchMeta = m.branch
    ? `${esc(m.branch)} · ${total} changed`
    : `${total} changed`;

  pane.innerHTML =
    head("Changes", branchMeta) +
    '<div class="preview-body"><div class="git-panel">' +
    changesHtml +
    '<h3 class="commits-h">Recent commits</h3>' +
    `<ul class="commitlist">${commits}</ul>` +
    "</div></div>";
}

function openGit() {
  state.selected = null;
  state.view = "git";
  renderTree();
  renderGit();
  $("preview").scrollTop = 0;
  loadLog();
}

function select(path, mode = "content") {
  state.view = "preview";
  state.selected = path;
  // make sure the tree reveals the selection
  const parts = path.split("/");
  let acc = "";
  for (let i = 0; i < parts.length - 1; i++) {
    acc = acc ? `${acc}/${parts[i]}` : parts[i];
    state.expanded.add(acc);
  }
  renderTree();
  if (mode === "diff") showDiff(path, true);
  else showPreview(path, true);
}

async function showPreview(path, resetScroll) {
  const pane = $("preview");
  state.view = "preview";
  state.mode = "content";
  const e = ext(path);
  const keepScroll = resetScroll ? 0 : pane.scrollTop;
  const actions = statusFor(path)
    ? '<button class="btn" data-action="diff">Diff</button>'
    : "";

  const render = (html) => {
    pane.innerHTML = html;
    /* Replacing innerHTML keeps the old scroll offset, so set it explicitly:
     * a fresh document starts at the top, a live reload keeps your place. */
    pane.scrollTop = resetScroll ? 0 : keepScroll;
  };

  try {
    if (MD.has(e)) {
      const res = await fetch(`/api/md?path=${encodeURIComponent(path)}`);
      if (!res.ok) throw new Error(res.statusText);
      const html = await res.text();
      render(head(path, "markdown", actions) + `<div class="preview-body"><article class="markdown">${html}</article></div>`);
    } else if (IMG.has(e)) {
      render(
        head(path, "image", actions) +
          `<div class="preview-body center"><img class="media" alt="${esc(path)}" src="${rawUrl(path)}"></div>`
      );
    } else if (VID.has(e)) {
      render(
        head(path, "video", actions) +
          `<div class="preview-body center"><video class="media" controls preload="metadata" src="${rawUrl(path)}"></video></div>`
      );
    } else if (AUD.has(e)) {
      render(
        head(path, "audio", actions) +
          `<div class="preview-body center"><audio class="media" controls src="${rawUrl(path)}"></audio></div>`
      );
    } else if (PDF.has(e)) {
      render(
        head(path, "pdf", actions) +
          `<div class="preview-body"><iframe class="media" src="${rawUrl(path)}"></iframe></div>`
      );
    } else {
      const res = await fetch(`/api/code?path=${encodeURIComponent(path)}`);
      if (!res.ok) throw new Error(res.statusText);
      const html = await res.text();
      const lang = res.headers.get("x-gander-lang") || "text";
      render(head(path, lang, actions) + `<div class="preview-body">${html}</div>`);
    }
  } catch (err) {
    render(head(path) + `<div class="preview-body"><p>Could not preview: ${esc(String(err))}</p></div>`);
  }
}

/* Show the working-tree diff for a file, with a button back to the preview. */
async function showDiff(path, resetScroll) {
  const pane = $("preview");
  state.view = "preview";
  state.mode = "diff";
  const keepScroll = resetScroll ? 0 : pane.scrollTop;
  try {
    const res = await fetch(`/api/diff?path=${encodeURIComponent(path)}`);
    if (!res.ok) throw new Error(res.statusText);
    const html = await res.text();
    pane.innerHTML =
      head(path, "diff", '<button class="btn" data-action="file">View file</button>') +
      `<div class="preview-body">${html}</div>`;
  } catch (err) {
    pane.innerHTML =
      head(path) +
      `<div class="preview-body"><p>Could not show diff: ${esc(String(err))}</p></div>`;
  }
  pane.scrollTop = resetScroll ? 0 : keepScroll;
}

/* ---------------------------------------------------------------- boot ---- */

async function loadTree() {
  const res = await fetch("/api/tree");
  if (!res.ok) return;
  const data = await res.json();
  state.files = data.files || [];
  state.isGit = !!data.is_git;
  $("repo-name").textContent = data.name || "gander";
  document.title = `${data.name || "gander"} · gander`;
  const br = $("repo-branch");
  if (data.is_git && data.branch) {
    br.textContent = data.branch;
    br.hidden = false;
  } else {
    br.hidden = true;
  }
  updateChangesButton();
  renderTree();
}

async function loadMeta() {
  try {
    const res = await fetch("/api/meta");
    if (!res.ok) return;
    state.meta = await res.json();
  } catch (_) {
    state.meta = null;
  }
  if (!state.selected && state.view === "preview") renderHome(false);
}

async function loadStatus() {
  try {
    const res = await fetch("/api/status");
    if (res.ok) {
      const data = await res.json();
      state.status = new Map((data.files || []).map((f) => [f.path, f]));
    } else {
      state.status = new Map();
    }
  } catch (_) {
    state.status = new Map();
  }
  updateChangesButton();
  renderTree();
  if (state.view === "git") renderGit();
}

async function loadLog() {
  try {
    const res = await fetch("/api/log");
    if (res.ok) {
      const data = await res.json();
      state.log = data.commits || [];
    } else {
      state.log = [];
    }
  } catch (_) {
    state.log = [];
  }
  if (state.view === "git") renderGit();
}

function updateChangesButton() {
  const btn = $("changes-btn");
  if (!state.isGit) {
    btn.hidden = true;
    return;
  }
  const n = state.status.size;
  btn.hidden = false;
  btn.textContent = n ? `Changes ${n}` : "Changes";
  btn.classList.toggle("has-changes", n > 0);
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
    const gitChange = changed.some((p) => p.startsWith(".git/"));
    await Promise.all([loadTree(), loadStatus(), loadMeta()]);
    if (state.view === "git") await loadLog();
    const sel = state.selected;
    if (sel && (gitChange || changed.includes(sel) || changed.length === 0)) {
      if (state.mode === "diff") showDiff(sel, false);
      else showPreview(sel, false);
    }
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

document.querySelector(".brand").addEventListener("click", showHome);
$("changes-btn").addEventListener("click", openGit);

/* One delegated handler for everything rendered into the preview pane. */
$("preview").addEventListener("click", (e) => {
  const t = e.target.closest("[data-action]");
  if (t) {
    const action = t.dataset.action;
    if (action === "git") openGit();
    else if (action === "diff" && state.selected) showDiff(state.selected, true);
    else if (action === "file" && state.selected) showPreview(state.selected, true);
    else if (action === "select" && t.dataset.path) select(t.dataset.path, "diff");
    return;
  }
  /* A relative link in Markdown points at /raw/<path>; preview it here rather
   * than navigating away to the raw file. */
  const a = e.target.closest("a[href^='/raw/']");
  if (a) {
    e.preventDefault();
    let p = a.getAttribute("href").slice("/raw/".length);
    const cut = p.search(/[#?]/);
    if (cut >= 0) p = p.slice(0, cut);
    try {
      p = p.split("/").map(decodeURIComponent).join("/");
    } catch (_) {}
    if (p) select(p);
  }
});

loadTree().then(loadStatus).then(loadMeta);
startEvents();
