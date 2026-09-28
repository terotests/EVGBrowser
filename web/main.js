// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The web host: EVG Browser inside a browser tab.
//
// What is here is only what a browser tab knows and the Ranger app cannot:
// the window's size, the pointer, the wheel, the keyboard, `fetch`, image
// decoding and an <svg> to paint into. Everything else — the toolbar, the
// history, HTML and CSS, layout, hit testing, scrolling — is the compiled
// Ranger program (`evg_browser.js`), the same code a native host runs.

import { renderDisplayList } from "./evg-html.js";
import { installCanvasMeasurer } from "./evg-measure.js";
import { createFetcher, detectProxy } from "./fetchers.js";

const mod = globalThis.EvgBrowser;
const stage = document.getElementById("stage");
const ime = document.getElementById("ime");
const errBox = document.getElementById("err");

function showError(e) {
  errBox.hidden = false;
  errBox.textContent = String((e && e.stack) || e);
}

// The browser measures the text, with the face the painter will draw it in.
// Installed before the app exists: layouts read the default when made.
const fonts = installCanvasMeasurer(mod);

const params = new URLSearchParams(location.search);
const coarse = matchMedia("(pointer: coarse)").matches;
const host = new mod.BrowserHost();
const app = host.browser;
if (params.get("images") === "0") app.setImagesEnabled(false);
if (params.get("js") === "0") app.setScriptsEnabled(false);
// ?open=1: no allowlist (locally with `EVG_OPEN=1 npm run serve`, whose
// proxy then fetches any site too; on Pages CORS still decides)
if (params.get("open") === "1") app.setAllowlistEnabled(false);
// the sample pages that ship with the demo, on this page's own origin
app.setSamplesBase(new URL("samples/", location.href).href);

const images = new Map();
let fetchText = createFetcher({});
let dirty = true;

// --- requests ---------------------------------------------------------------

function pump() {
  for (;;) {
    const line = app.nextRequest();
    if (!line) break;
    const [idText, kind, url] = line.split("\t");
    const id = Number(idText);
    if (kind === "image") {
      loadImage(url);
      continue;
    }
    fetchText(url, kind).then((r) => {
      app.deliver(id, r.status | 0, r.finalUrl || "", r.contentType || "", r.body || "");
      dirty = true;
      pump();
      syncTitle();
    }, (e) => {
      app.deliver(id, 0, "", "", String(e));
      dirty = true;
    });
  }
}

function loadImage(url) {
  if (images.has(url)) {
    const img = images.get(url);
    if (img) app.imageLoaded(url, img.naturalWidth, img.naturalHeight);
    else app.imageFailed(url);
    return;
  }
  const img = new Image();
  // no crossOrigin: the SVG painter only shows the picture, it never reads
  // its pixels, so a site that sends no CORS headers still works
  img.decoding = "async";
  img.referrerPolicy = "no-referrer";
  img.onload = () => {
    images.set(url, img);
    app.imageLoaded(url, img.naturalWidth || 1, img.naturalHeight || 1);
    dirty = true;
  };
  img.onerror = () => {
    images.set(url, null);
    app.imageFailed(url);
    dirty = true;
  };
  img.src = url;
}

function syncTitle() {
  const t = app.pageTitle();
  document.title = t ? `${t} — EVG Browser` : "EVG Browser";
  const u = app.currentUrl();
  const shown = new URL(location.href);
  if (u && u !== "about:home") shown.searchParams.set("url", u);
  else shown.searchParams.delete("url");
  if (shown.href !== location.href) history.replaceState(null, "", shown);
}

// --- the script realm -------------------------------------------------------
//
// A page's scripts run in a Web Worker (realm-worker.js), on a copy of the
// page; the DOM ops they produce come back as text. The worker is started
// the first time a page has scripts, and terminated — with the page left as
// the server sent it — when a job takes longer than REALM_LIMIT_MS.

const REALM_LIMIT_MS = 8000;
let realmWorker = null;
let realmTimer = 0;
let realmJob = -1;

function stopRealm() {
  if (realmWorker) realmWorker.terminate();
  realmWorker = null;
  clearTimeout(realmTimer);
}

function runRealm() {
  if (!app.realmJobPending()) return;
  const job = app.takeRealmJob();
  const scripts = [];
  for (let i = 0; i < app.realmScriptCount(); i++) scripts.push({ url: app.realmScriptUrl(i), text: app.realmScriptText(i) });
  if (realmJob >= 0) stopRealm();  // a new page: the old job is abandoned
  if (!realmWorker) {
    realmWorker = new Worker("./realm-worker.js");
    realmWorker.onmessage = (e) => {
      const r = e.data;
      if (r.job !== realmJob) return;
      clearTimeout(realmTimer);
      realmJob = -1;
      if (r.ok) app.realmResult(r.job, r.ops, r.summary);
      else app.realmFailed(r.job, r.error);
      window.__realm = r;
      dirty = true;
      pump();
    };
    realmWorker.onerror = (e) => {
      const j = realmJob;
      stopRealm();
      realmJob = -1;
      app.realmFailed(j, String(e.message || e));
      dirty = true;
    };
  }
  realmJob = job;
  realmTimer = setTimeout(() => {
    stopRealm();
    realmJob = -1;
    app.realmFailed(job, "aikaraja");
    window.__realm = { job, ok: false, error: "timeout" };
    dirty = true;
  }, REALM_LIMIT_MS);
  realmWorker.postMessage({ job, html: app.realmHtml(), url: app.realmUrl(), width: app.realmWidth(), height: app.realmHeight(), scripts });
}

// --- painting ---------------------------------------------------------------

let paintedFocus = "";

function paint() {
  const doc = JSON.parse(app.frameJson());
  renderDisplayList(stage, doc, { images });
  dirty = false;
  stage.style.cursor = app.hoveredId() && app.hoveredId() !== "" ? "pointer" : "default";
  syncFocus();
}

// The drawn field and the real <input> agree about what is being edited.
function syncFocus() {
  const f = host.focusedField();
  if (f === paintedFocus) return;
  paintedFocus = f;
  if (f) {
    ime.value = app.fieldValue(f);
    ime.focus({ preventScroll: true });
    if (f === "addr") ime.select();
  } else if (document.activeElement === ime) {
    ime.blur();
    stage.focus({ preventScroll: true });
  }
}

let last = performance.now();
function frame(now) {
  const dt = Math.min(64, now - last);
  last = now;
  try {
    if (host.tick(dt)) dirty = true;
    pump();
    runRealm();
    if (dirty) paint();
  } catch (e) {
    showError(e);
  }
  requestAnimationFrame(frame);
}

function resize() {
  const w = Math.max(280, stage.clientWidth), h = Math.max(320, stage.clientHeight);
  host.resize(w, h);
  dirty = true;
}

// --- input ------------------------------------------------------------------

const pointers = new Map();
let lastMoveT = 0;

function local(e) {
  const r = stage.getBoundingClientRect();
  return [e.clientX - r.left, e.clientY - r.top];
}

stage.addEventListener("pointerdown", (e) => {
  const [x, y] = local(e);
  pointers.set(e.pointerId, { x, y, type: e.pointerType });
  lastMoveT = e.timeStamp;
  stage.setPointerCapture(e.pointerId);
  host.pressAt(x, y);
  dirty = true;
});

stage.addEventListener("pointermove", (e) => {
  const [x, y] = local(e);
  const p = pointers.get(e.pointerId);
  if (!p) {
    if (e.pointerType === "mouse" && host.hoverAt(x, y)) dirty = true;
    return;
  }
  // a finger (or a pen) drags the page; a mouse button held down does too,
  // since there is no text to select
  const dx = x - p.x, dy = y - p.y;
  p.x = x; p.y = y;
  const dt = Math.max(1, e.timeStamp - lastMoveT);
  lastMoveT = e.timeStamp;
  if (host.panAt(dx, dy, dt)) dirty = true;
});

function lift(e, cancel) {
  if (!pointers.has(e.pointerId)) return;
  pointers.delete(e.pointerId);
  if (cancel) host.cancelPress(); else host.releasePress();
  dirty = true;
  // inside the gesture, or a phone will not raise its keyboard
  syncFocus();
  pump();
}
stage.addEventListener("pointerup", (e) => lift(e, false));
stage.addEventListener("pointercancel", (e) => lift(e, true));
stage.addEventListener("pointerleave", () => { if (host.clearHover()) dirty = true; });

stage.addEventListener("wheel", (e) => {
  e.preventDefault();
  const unit = e.deltaMode === 1 ? 32 : e.deltaMode === 2 ? stage.clientHeight : 1;
  if (host.wheel(e.deltaY * unit)) dirty = true;
}, { passive: false });

// Keys the app handles itself. Text goes through the real <input> while a
// field is focused; everything else — scrolling, Enter, Escape — is a name.
const NAMED = new Set(["Enter", "Escape", "Backspace", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight",
  "PageUp", "PageDown", "Home", "End", " "]);

function onKey(e) {
  const inField = !!host.focusedField();
  const ctrl = e.ctrlKey || e.metaKey;
  if (inField) {
    // the <input> edits; the app hears only the keys that end the edit
    if (e.key === "Enter" || e.key === "Escape") {
      e.preventDefault();
      app.setFieldValue(host.focusedField(), ime.value);
      host.key(e.key, e.shiftKey, ctrl);
      dirty = true;
      pump();
    }
    return;
  }
  if (e.altKey && (e.key === "ArrowLeft" || e.key === "ArrowRight")) {
    e.preventDefault();
    if (e.key === "ArrowLeft") app.back(); else app.forward();
    dirty = true;
    pump();
    return;
  }
  if (NAMED.has(e.key) || (ctrl && (e.key === "l" || e.key === "r"))) {
    if (host.key(e.key, e.shiftKey, ctrl)) {
      e.preventDefault();
      dirty = true;
      pump();
    }
  }
}
document.addEventListener("keydown", onKey);

ime.addEventListener("input", () => {
  const f = host.focusedField();
  if (!f) return;
  app.setFieldValue(f, ime.value);
  dirty = true;
});
ime.addEventListener("blur", () => {
  // a tap elsewhere on the page ends the edit; the next paint agrees
  setTimeout(() => {
    if (document.activeElement !== ime && host.focusedField()) {
      host.blur();
      paintedFocus = "";
      dirty = true;
    }
  }, 0);
});

// --- start ------------------------------------------------------------------

async function start() {
  const proxy = params.get("proxy") || (await detectProxy());
  fetchText = createFetcher({ proxy });
  const w = Math.max(280, stage.clientWidth), h = Math.max(320, stage.clientHeight);
  host.startAt(w, h, coarse, params.get("url") || "about:home");
  new ResizeObserver(resize).observe(stage);
  if (document.fonts && document.fonts.ready) {
    document.fonts.ready.then(() => { fonts.refresh(); app.rebuild(); dirty = true; });
  }
  stage.focus({ preventScroll: true });
  pump();
  requestAnimationFrame(frame);
  // for checks and the console
  window.__evgBrowser = { host, app, images, paint };
}

start().catch(showError);
