// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The DOM a page's scripts see inside the realm (src/js/JsRealm.rgr).
//
// Everything here is built on ONE host function, __dom(op, ...), which reads
// and edits the realm's copy of the document; every edit becomes an op that
// is sent to the browser. There is deliberately no network, no storage that
// outlives the page, no dialogs and no windows: fetch and XMLHttpRequest fail,
// alert/confirm/prompt/open do nothing. Timers run after the page's scripts,
// on a virtual clock, a bounded number of them.
//
// src/js/JsPrelude.rgr is generated from this file (tools/gen-prelude.mjs).

var __nodes = {};
var __listeners = {};
var __timers = [];
var __timerSeq = 1;
var __clock = 0;

function __wrap(id) {
  if (id === null || id === undefined || id < 0) return null;
  var n = __nodes[id];
  if (n) return n;
  var t = __dom("type", id);
  if (t === 3) n = new Text(id);
  else if (t === 9) n = new HTMLDocument(id);
  else n = new HTMLElement(id);
  __nodes[id] = n;
  return n;
}

function __list(csv) {
  var out = [];
  if (csv === "" || csv === null || csv === undefined) return out;
  var parts = String(csv).split(",");
  for (var i = 0; i < parts.length; i++) out.push(__wrap(Number(parts[i])));
  return out;
}

function __nodeArg(x) {
  if (x === null || x === undefined) return -1;
  if (typeof x === "string") return __dom("createText", x);
  return x.__id;
}

// --- Node -------------------------------------------------------------------

function Node(id) { this.__id = id; }
Node.ELEMENT_NODE = 1;
Node.TEXT_NODE = 3;
Node.DOCUMENT_NODE = 9;

Node.prototype.appendChild = function (c) {
  if (c && c.__fragment) { var k = c.childNodes; for (var i = 0; i < k.length; i++) this.appendChild(k[i]); return c; }
  __dom("insert", this.__id, __nodeArg(c), -1);
  return c;
};
Node.prototype.insertBefore = function (c, ref) {
  if (c && c.__fragment) { var k = c.childNodes; for (var i = 0; i < k.length; i++) this.insertBefore(k[i], ref); return c; }
  __dom("insert", this.__id, __nodeArg(c), ref ? ref.__id : -1);
  return c;
};
Node.prototype.removeChild = function (c) { if (c) __dom("remove", c.__id); return c; };
Node.prototype.replaceChild = function (n, old) { this.insertBefore(n, old); this.removeChild(old); return old; };
Node.prototype.remove = function () { __dom("remove", this.__id); };
Node.prototype.contains = function (o) { return !!o && __dom("contains", this.__id, o.__id) === 1; };
Node.prototype.hasChildNodes = function () { return __dom("kids", this.__id) !== ""; };
Node.prototype.cloneNode = function (deep) { return __wrap(__dom("clone", this.__id, deep ? 1 : 0)); };
Node.prototype.addEventListener = function (type, fn) {
  var key = this.__id + ":" + type;
  (__listeners[key] = __listeners[key] || []).push(fn);
};
Node.prototype.removeEventListener = function (type, fn) {
  var l = __listeners[this.__id + ":" + type];
  if (!l) return;
  for (var i = 0; i < l.length; i++) if (l[i] === fn) { l.splice(i, 1); return; }
};
Node.prototype.dispatchEvent = function (ev) { __fire(this, ev.type, ev); return true; };
Node.prototype.append = function () { for (var i = 0; i < arguments.length; i++) this.appendChild(arguments[i]); };
Node.prototype.prepend = function () { var f = this.firstChild; for (var i = 0; i < arguments.length; i++) this.insertBefore(__wrap(__nodeArg(arguments[i])), f); };

function __getter(proto, name, get, set) {
  Object.defineProperty(proto, name, { get: get, set: set || function () {}, configurable: true });
}

__getter(Node.prototype, "nodeType", function () { return __dom("type", this.__id); });
__getter(Node.prototype, "parentNode", function () { return __wrap(__dom("parent", this.__id)); });
__getter(Node.prototype, "parentElement", function () { var p = __wrap(__dom("parent", this.__id)); return p && p.nodeType === 1 ? p : null; });
__getter(Node.prototype, "childNodes", function () { return __list(__dom("kids", this.__id)); });
__getter(Node.prototype, "firstChild", function () { var k = this.childNodes; return k.length ? k[0] : null; });
__getter(Node.prototype, "lastChild", function () { var k = this.childNodes; return k.length ? k[k.length - 1] : null; });
__getter(Node.prototype, "nextSibling", function () { return __wrap(__dom("sibling", this.__id, 1, 0)); });
__getter(Node.prototype, "previousSibling", function () { return __wrap(__dom("sibling", this.__id, -1, 0)); });
__getter(Node.prototype, "ownerDocument", function () { return document; });
__getter(Node.prototype, "isConnected", function () { return __dom("connected", this.__id) === 1; });
__getter(Node.prototype, "textContent",
  function () { return __dom("text", this.__id); },
  function (v) { __dom("setText", this.__id, v === null || v === undefined ? "" : String(v)); });

// --- Text -------------------------------------------------------------------

function Text(id) { Node.call(this, id); }
Text.prototype = Object.create(Node.prototype);
Text.prototype.constructor = Text;
__getter(Text.prototype, "nodeName", function () { return "#text"; });
__getter(Text.prototype, "data", function () { return __dom("text", this.__id); }, function (v) { __dom("setText", this.__id, String(v)); });
__getter(Text.prototype, "nodeValue", function () { return __dom("text", this.__id); }, function (v) { __dom("setText", this.__id, String(v)); });

// --- Element ----------------------------------------------------------------

function Element(id) { Node.call(this, id); }
Element.prototype = Object.create(Node.prototype);
Element.prototype.constructor = Element;
function HTMLElement(id) { Element.call(this, id); }
HTMLElement.prototype = Object.create(Element.prototype);
HTMLElement.prototype.constructor = HTMLElement;

Element.prototype.getAttribute = function (n) { return __dom("attr", this.__id, String(n)); };
Element.prototype.setAttribute = function (n, v) { __dom("setAttr", this.__id, String(n), String(v)); };
Element.prototype.removeAttribute = function (n) { __dom("rmAttr", this.__id, String(n)); };
Element.prototype.hasAttribute = function (n) { return __dom("attr", this.__id, String(n)) !== null; };
Element.prototype.toggleAttribute = function (n, force) {
  var has = this.hasAttribute(n);
  var want = force === undefined ? !has : !!force;
  if (want && !has) this.setAttribute(n, "");
  if (!want && has) this.removeAttribute(n);
  return want;
};
Element.prototype.setAttributeNS = function (ns, n, v) { this.setAttribute(n, v); };
Element.prototype.getAttributeNames = function () { var s = __dom("attrs", this.__id); return s === "" ? [] : s.split("\n"); };
Element.prototype.querySelector = function (s) { return __wrap(__dom("qs", this.__id, String(s))); };
Element.prototype.querySelectorAll = function (s) { return __list(__dom("qsa", this.__id, String(s))); };
Element.prototype.getElementsByTagName = function (t) { return __list(__dom("qsa", this.__id, t === "*" ? "*" : String(t))); };
Element.prototype.getElementsByClassName = function (c) { return __list(__dom("qsa", this.__id, "." + String(c).trim().split(/\s+/).join("."))); };
Element.prototype.matches = function (s) { return __dom("match", this.__id, String(s)) === 1; };
Element.prototype.closest = function (s) {
  var e = this;
  while (e && e.nodeType === 1) { if (e.matches(s)) return e; e = e.parentNode; }
  return null;
};
Element.prototype.getBoundingClientRect = function () { return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 }; };
Element.prototype.getClientRects = function () { return []; };
Element.prototype.focus = function () {};
Element.prototype.blur = function () {};
Element.prototype.click = function () { __fire(this, "click", new Event("click")); };
Element.prototype.scrollIntoView = function () {};
Element.prototype.insertAdjacentHTML = function (where, html) {
  var tmp = document.createElement("div");
  tmp.innerHTML = html;
  var kids = tmp.childNodes;
  var w = String(where).toLowerCase();
  for (var i = 0; i < kids.length; i++) {
    if (w === "beforeend") this.appendChild(kids[i]);
    else if (w === "afterbegin") this.insertBefore(kids[i], this.firstChild);
    else if (w === "beforebegin" && this.parentNode) this.parentNode.insertBefore(kids[i], this);
    else if (w === "afterend" && this.parentNode) this.parentNode.insertBefore(kids[i], this.nextSibling);
  }
};
Element.prototype.attachShadow = function () { return this; };

__getter(Element.prototype, "tagName", function () { return __dom("tag", this.__id).toUpperCase(); });
__getter(Element.prototype, "nodeName", function () { return __dom("tag", this.__id).toUpperCase(); });
__getter(Element.prototype, "localName", function () { return __dom("tag", this.__id); });
__getter(Element.prototype, "id", function () { return this.getAttribute("id") || ""; }, function (v) { this.setAttribute("id", v); });
__getter(Element.prototype, "className", function () { return this.getAttribute("class") || ""; }, function (v) { this.setAttribute("class", v); });
__getter(Element.prototype, "children", function () { return __list(__dom("kids", this.__id, 1)); });
__getter(Element.prototype, "childElementCount", function () { return this.children.length; });
__getter(Element.prototype, "firstElementChild", function () { var k = this.children; return k.length ? k[0] : null; });
__getter(Element.prototype, "lastElementChild", function () { var k = this.children; return k.length ? k[k.length - 1] : null; });
__getter(Element.prototype, "nextElementSibling", function () { return __wrap(__dom("sibling", this.__id, 1, 1)); });
__getter(Element.prototype, "previousElementSibling", function () { return __wrap(__dom("sibling", this.__id, -1, 1)); });
__getter(Element.prototype, "innerHTML", function () { return __dom("inner", this.__id); }, function (v) { __dom("setInner", this.__id, String(v)); });
__getter(Element.prototype, "outerHTML", function () { return __dom("outer", this.__id); });
__getter(Element.prototype, "innerText", function () { return __dom("text", this.__id); }, function (v) { __dom("setText", this.__id, String(v)); });
__getter(Element.prototype, "hidden", function () { return this.hasAttribute("hidden"); }, function (v) { this.toggleAttribute("hidden", !!v); });
__getter(Element.prototype, "href", function () { return __dom("url", this.__id, "href"); }, function (v) { this.setAttribute("href", v); });
__getter(Element.prototype, "src", function () { return __dom("url", this.__id, "src"); }, function (v) { this.setAttribute("src", v); });
__getter(Element.prototype, "title", function () { return this.getAttribute("title") || ""; }, function (v) { this.setAttribute("title", v); });
__getter(Element.prototype, "alt", function () { return this.getAttribute("alt") || ""; }, function (v) { this.setAttribute("alt", v); });
__getter(Element.prototype, "value", function () { return this.getAttribute("value") || ""; }, function (v) { this.setAttribute("value", v); });
__getter(Element.prototype, "type", function () { return this.getAttribute("type") || ""; });
__getter(Element.prototype, "name", function () { return this.getAttribute("name") || ""; });
__getter(Element.prototype, "checked", function () { return this.hasAttribute("checked"); }, function (v) { this.toggleAttribute("checked", !!v); });
__getter(Element.prototype, "disabled", function () { return this.hasAttribute("disabled"); }, function (v) { this.toggleAttribute("disabled", !!v); });
__getter(Element.prototype, "offsetWidth", function () { return 0; });
__getter(Element.prototype, "offsetHeight", function () { return 0; });
__getter(Element.prototype, "clientWidth", function () { return 0; });
__getter(Element.prototype, "clientHeight", function () { return 0; });
__getter(Element.prototype, "scrollTop", function () { return 0; });
__getter(Element.prototype, "classList", function () { return new DOMTokenList(this); });
__getter(Element.prototype, "dataset", function () { return __dataset(this); });
__getter(Element.prototype, "style", function () { return new CSSStyleDeclaration(this); });

// class="…" as a list
function DOMTokenList(el) { this.__el = el; }
DOMTokenList.prototype.__get = function () { var c = this.__el.className.trim(); return c === "" ? [] : c.split(/\s+/); };
DOMTokenList.prototype.__set = function (l) { this.__el.className = l.join(" "); };
DOMTokenList.prototype.contains = function (c) { return this.__get().indexOf(c) >= 0; };
DOMTokenList.prototype.add = function () {
  var l = this.__get();
  for (var i = 0; i < arguments.length; i++) if (l.indexOf(arguments[i]) < 0) l.push(arguments[i]);
  this.__set(l);
};
DOMTokenList.prototype.remove = function () {
  var l = this.__get(), out = [];
  for (var i = 0; i < l.length; i++) {
    var keep = true;
    for (var j = 0; j < arguments.length; j++) if (l[i] === arguments[j]) keep = false;
    if (keep) out.push(l[i]);
  }
  this.__set(out);
};
DOMTokenList.prototype.toggle = function (c, force) {
  var has = this.contains(c);
  var want = force === undefined ? !has : !!force;
  if (want && !has) this.add(c);
  if (!want && has) this.remove(c);
  return want;
};
DOMTokenList.prototype.replace = function (a, b) { if (this.contains(a)) { this.remove(a); this.add(b); } };
__getter(DOMTokenList.prototype, "length", function () { return this.__get().length; });
DOMTokenList.prototype.item = function (i) { return this.__get()[i] || null; };

// data-* as an object, read and written through
function __dataset(el) {
  var d = {};
  var names = el.getAttributeNames();
  for (var i = 0; i < names.length; i++) {
    if (names[i].indexOf("data-") === 0) {
      var key = names[i].slice(5).replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); });
      d[key] = el.getAttribute(names[i]);
    }
  }
  return d;
}

// el.style: the style attribute, a property at a time
var __styleProps = ["display", "visibility", "color", "backgroundColor", "background", "width", "height",
  "minWidth", "maxWidth", "minHeight", "maxHeight", "margin", "marginTop", "marginRight", "marginBottom",
  "marginLeft", "padding", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft", "border",
  "borderRadius", "fontSize", "fontWeight", "lineHeight", "textAlign", "opacity", "position", "top",
  "left", "right", "bottom", "overflow", "flex", "flexDirection", "flexWrap", "justifyContent",
  "alignItems", "gap", "transform", "transition", "zIndex", "cursor", "whiteSpace", "fontFamily"];
function __kebab(p) { return p.replace(/[A-Z]/g, function (c) { return "-" + c.toLowerCase(); }); }
function __parseStyle(s) {
  var out = {}, parts = (s || "").split(";");
  for (var i = 0; i < parts.length; i++) {
    var k = parts[i].indexOf(":");
    if (k > 0) out[parts[i].slice(0, k).trim()] = parts[i].slice(k + 1).trim();
  }
  return out;
}
function __writeStyle(el, map) {
  var s = [];
  for (var k in map) if (map[k] !== "" && map[k] !== null && map[k] !== undefined) s.push(k + ": " + map[k]);
  if (s.length) el.setAttribute("style", s.join("; ")); else el.removeAttribute("style");
}
function CSSStyleDeclaration(el) { this.__el = el; }
CSSStyleDeclaration.prototype.setProperty = function (k, v) {
  var m = __parseStyle(this.__el.getAttribute("style"));
  m[k] = String(v);
  __writeStyle(this.__el, m);
};
CSSStyleDeclaration.prototype.getPropertyValue = function (k) { return __parseStyle(this.__el.getAttribute("style"))[k] || ""; };
CSSStyleDeclaration.prototype.removeProperty = function (k) {
  var m = __parseStyle(this.__el.getAttribute("style"));
  var old = m[k] || "";
  delete m[k];
  __writeStyle(this.__el, m);
  return old;
};
__getter(CSSStyleDeclaration.prototype, "cssText",
  function () { return this.__el.getAttribute("style") || ""; },
  function (v) { this.__el.setAttribute("style", String(v)); });
for (var __si = 0; __si < __styleProps.length; __si++) {
  (function (p) {
    __getter(CSSStyleDeclaration.prototype, p,
      function () { return this.getPropertyValue(__kebab(p)); },
      function (v) { this.setProperty(__kebab(p), v === null || v === undefined ? "" : String(v)); });
  })(__styleProps[__si]);
}

// --- the document -----------------------------------------------------------

function HTMLDocument(id) { Node.call(this, id); }
HTMLDocument.prototype = Object.create(Node.prototype);
HTMLDocument.prototype.constructor = HTMLDocument;
HTMLDocument.prototype.getElementById = function (i) { return __wrap(__dom("byId", String(i))); };
HTMLDocument.prototype.querySelector = function (s) { return __wrap(__dom("qs", 0, String(s))); };
HTMLDocument.prototype.querySelectorAll = function (s) { return __list(__dom("qsa", 0, String(s))); };
HTMLDocument.prototype.getElementsByTagName = function (t) { return __list(__dom("qsa", 0, String(t))); };
HTMLDocument.prototype.getElementsByClassName = function (c) { return __list(__dom("qsa", 0, "." + String(c).trim().split(/\s+/).join("."))); };
HTMLDocument.prototype.getElementsByName = function (n) { return __list(__dom("qsa", 0, "[name=\"" + n + "\"]")); };
HTMLDocument.prototype.createElement = function (t) { return __wrap(__dom("create", String(t))); };
HTMLDocument.prototype.createElementNS = function (ns, t) { return __wrap(__dom("create", String(t))); };
HTMLDocument.prototype.createTextNode = function (s) { return __wrap(__dom("createText", String(s))); };
HTMLDocument.prototype.createComment = function () { return __wrap(__dom("createText", "")); };
HTMLDocument.prototype.createDocumentFragment = function () {
  var f = __wrap(__dom("create", "div"));
  f.__fragment = true;
  return f;
};
HTMLDocument.prototype.createEvent = function () { return new Event(""); };
HTMLDocument.prototype.write = function () {};
HTMLDocument.prototype.writeln = function () {};
HTMLDocument.prototype.hasFocus = function () { return true; };
__getter(HTMLDocument.prototype, "documentElement", function () { return __wrap(__dom("find", "html")); });
__getter(HTMLDocument.prototype, "head", function () { return __wrap(__dom("find", "head")); });
__getter(HTMLDocument.prototype, "body", function () { return __wrap(__dom("find", "body")); });
__getter(HTMLDocument.prototype, "title", function () { return __dom("title"); }, function (v) { __dom("setTitle", String(v)); });
__getter(HTMLDocument.prototype, "nodeName", function () { return "#document"; });
__getter(HTMLDocument.prototype, "readyState", function () { return __readyState; });
__getter(HTMLDocument.prototype, "cookie", function () { return ""; }, function () {});
__getter(HTMLDocument.prototype, "visibilityState", function () { return "visible"; });
__getter(HTMLDocument.prototype, "hidden", function () { return false; });
__getter(HTMLDocument.prototype, "currentScript", function () { return __wrap(__currentScript); });
__getter(HTMLDocument.prototype, "location", function () { return location; });
__getter(HTMLDocument.prototype, "defaultView", function () { return window; });
__getter(HTMLDocument.prototype, "referrer", function () { return ""; });

var __readyState = "loading";
var __currentScript = -1;
var document = __wrap(0);

// --- events -----------------------------------------------------------------

function Event(type, init) {
  this.type = type;
  this.bubbles = !!(init && init.bubbles);
  this.defaultPrevented = false;
  this.target = null;
  this.detail = init && init.detail;
}
Event.prototype.preventDefault = function () { this.defaultPrevented = true; };
Event.prototype.stopPropagation = function () {};
Event.prototype.stopImmediatePropagation = function () {};
function CustomEvent(type, init) { Event.call(this, type, init); }
CustomEvent.prototype = Object.create(Event.prototype);

function __fire(target, type, ev) {
  var l = __listeners[(target ? target.__id : "w") + ":" + type];
  if (!l) return;
  ev = ev || new Event(type);
  if (!ev.target) ev.target = target;
  ev.currentTarget = target;
  var copy = l.slice();
  for (var i = 0; i < copy.length; i++) {
    try { copy[i].call(target || window, ev); } catch (e) { __dom("error", String(e && e.message || e)); }
  }
}

// --- the window -------------------------------------------------------------

var window = globalThis;
var self = globalThis;
window.addEventListener = function (type, fn) { (__listeners["w:" + type] = __listeners["w:" + type] || []).push(fn); };
window.removeEventListener = function () {};
window.dispatchEvent = function (ev) { __fire(null, ev.type, ev); return true; };

var __url = __dom("pageUrl");
var location = {
  href: __url, toString: function () { return __url; },
  protocol: __dom("urlPart", "protocol"), host: __dom("urlPart", "host"), hostname: __dom("urlPart", "hostname"),
  pathname: __dom("urlPart", "pathname"), search: __dom("urlPart", "search"), hash: __dom("urlPart", "hash"),
  origin: __dom("urlPart", "origin"), port: "",
  assign: function () {}, replace: function () {}, reload: function () {}
};
var navigator = { userAgent: "Mozilla/5.0 (EVG Browser) Safari", language: "fi-FI", languages: ["fi-FI", "fi", "en"],
  onLine: false, cookieEnabled: false, platform: "EVG", vendor: "", maxTouchPoints: 0,
  sendBeacon: function () { return false; } };
var screen = { width: __dom("viewport", "w"), height: __dom("viewport", "h") };
var innerWidth = __dom("viewport", "w");
var innerHeight = __dom("viewport", "h");
var outerWidth = innerWidth, outerHeight = innerHeight;
var devicePixelRatio = 1;
var scrollX = 0, scrollY = 0, pageXOffset = 0, pageYOffset = 0;
var history = { length: 1, state: null, pushState: function () {}, replaceState: function () {}, back: function () {}, forward: function () {}, go: function () {} };
function scrollTo() {}
function scroll() {}
function alert() {}
function confirm() { return false; }
function prompt() { return null; }
function open() { return null; }
function print() {}
function getComputedStyle(el) { return el && el.style ? el.style : new CSSStyleDeclaration(document.body); }
function matchMedia(q) {
  return { matches: __dom("media", String(q)) === 1, media: q,
    addListener: function () {}, removeListener: function () {},
    addEventListener: function () {}, removeEventListener: function () {} };
}

function __Storage() { this.__d = {}; }
__Storage.prototype.getItem = function (k) { return this.__d.hasOwnProperty(k) ? this.__d[k] : null; };
__Storage.prototype.setItem = function (k, v) { this.__d[k] = String(v); };
__Storage.prototype.removeItem = function (k) { delete this.__d[k]; };
__Storage.prototype.clear = function () { this.__d = {}; };
var localStorage = new __Storage();
var sessionStorage = new __Storage();

// no network from here: the page was fetched by the browser, and that is all
function fetch() { return Promise.reject(new TypeError("network is not available to page scripts")); }
function XMLHttpRequest() { this.readyState = 0; this.status = 0; this.responseText = ""; }
XMLHttpRequest.prototype.open = function () { this.readyState = 1; };
XMLHttpRequest.prototype.setRequestHeader = function () {};
XMLHttpRequest.prototype.addEventListener = function (t, fn) { this["on" + t] = fn; };
XMLHttpRequest.prototype.send = function () {
  var x = this;
  setTimeout(function () { x.readyState = 4; if (x.onerror) x.onerror(new Event("error")); if (x.onreadystatechange) x.onreadystatechange(); }, 0);
};
XMLHttpRequest.prototype.abort = function () {};
function WebSocket() { throw new Error("WebSocket is not available"); }
function EventSource() { throw new Error("EventSource is not available"); }

// timers: a virtual clock, advanced by the host after the scripts
function setTimeout(fn, ms) {
  if (typeof fn !== "function") return 0;
  var args = Array.prototype.slice.call(arguments, 2);
  var id = __timerSeq++;
  __timers.push({ id: id, at: __clock + (Number(ms) || 0), fn: fn, args: args, every: 0 });
  return id;
}
function setInterval(fn, ms) {
  var id = setTimeout(fn, ms);
  for (var i = 0; i < __timers.length; i++) if (__timers[i].id === id) __timers[i].every = Math.max(10, Number(ms) || 10);
  return id;
}
function clearTimeout(id) {
  for (var i = 0; i < __timers.length; i++) if (__timers[i].id === id) { __timers.splice(i, 1); return; }
}
var clearInterval = clearTimeout;
function requestAnimationFrame(fn) { return setTimeout(function () { fn(__clock); }, 16); }
var cancelAnimationFrame = clearTimeout;
function requestIdleCallback(fn) { return setTimeout(function () { fn({ didTimeout: false, timeRemaining: function () { return 10; } }); }, 1); }
var cancelIdleCallback = clearTimeout;
function queueMicrotask(fn) { Promise.resolve().then(fn); }
var performance = { now: function () { return __clock; }, mark: function () {}, measure: function () {}, getEntriesByType: function () { return []; } };

// Run due timers until `until` ms of virtual time, at most `budget` calls.
function __runTimers(until, budget) {
  var calls = 0;
  while (__timers.length && calls < budget) {
    var next = 0;
    for (var i = 1; i < __timers.length; i++) if (__timers[i].at < __timers[next].at) next = i;
    var t = __timers[next];
    if (t.at > until) break;
    __clock = t.at;
    if (t.every > 0) t.at = t.at + t.every; else __timers.splice(next, 1);
    calls++;
    try { t.fn.apply(window, t.args); } catch (e) { __dom("error", String(e && e.message || e)); }
  }
  return calls;
}

// observers: everything is visible at once, nothing ever resizes
function IntersectionObserver(cb) { this.__cb = cb; }
IntersectionObserver.prototype.observe = function (el) {
  var cb = this.__cb, self = this;
  setTimeout(function () { cb([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: el.getBoundingClientRect() }], self); }, 0);
};
IntersectionObserver.prototype.unobserve = function () {};
IntersectionObserver.prototype.disconnect = function () {};
function ResizeObserver() {}
ResizeObserver.prototype.observe = function () {};
ResizeObserver.prototype.unobserve = function () {};
ResizeObserver.prototype.disconnect = function () {};
function MutationObserver() {}
MutationObserver.prototype.observe = function () {};
MutationObserver.prototype.disconnect = function () {};
MutationObserver.prototype.takeRecords = function () { return []; };

var console = { log: function () {}, info: function () {}, warn: function () {}, debug: function () {},
  error: function () {}, trace: function () {}, group: function () {}, groupEnd: function () {}, table: function () {} };

// the host's steps, called in order by JsRealm
function __beforeScript(id) { __currentScript = id; }
function __domReady() {
  __readyState = "interactive";
  __currentScript = -1;
  __fire(document, "readystatechange");
  __fire(document, "DOMContentLoaded");
  __fire(null, "DOMContentLoaded");
}
function __loaded() {
  __readyState = "complete";
  __fire(document, "readystatechange");
  __fire(null, "load");
  __fire(null, "pageshow");
}

// One page script, in the global scope, as a <script> element runs it: an
// exception ends that script and not the ones after it.
function __runScript(src) {
  try { (0, eval)(src); } catch (e) { __dom("error", String((e && e.message) || e)); }
}
function __timerStep(until) { return __runTimers(until, 50); }
