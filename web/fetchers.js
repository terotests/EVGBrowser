// SPDX-License-Identifier: MIT
//
// How the web host fetches what the browser asks for.
//
// The Ranger app never does I/O: it queues requests and the host answers them.
// On a phone or a desktop that is the platform's HTTP client and nothing else.
// In a browser tab it is `fetch`, and `fetch` is subject to CORS: a page on
// github.io may read another site's response only when that site says so. Most
// sites do not (yle.fi does not), so this file tries, in order:
//
//   1. a proxy on the page's own origin, when one answers (`tools/serve.mjs`
//      is one, for running locally; it enforces the same allowlist);
//   2. adapters for sites that publish a CORS-enabled API for their content —
//      Wikipedia's action API, whose `parse` output is the article HTML;
//   3. a plain `fetch`, which works for the sites that allow it.
//
// When all of them fail the app is told why (status 0 and a message), and it
// shows its own error page with that message.

const TIMEOUT_MS = 15000;

function withTimeout(promise, ms) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error("aikakatkaisu")), ms);
    promise.then((v) => { clearTimeout(t); resolve(v); }, (e) => { clearTimeout(t); reject(e); });
  });
}

function escapeHtml(s) {
  return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

// --- Wikipedia --------------------------------------------------------------

const WIKI_CSS = `
.mw-editsection, .navbox, .vertical-navbox, .metadata, .noprint, .mw-jump-link, .sistersitebox, .ambox, .hatnote-nav { display: none; }
.infobox { background-color: #f6f8fb; border: 1px solid #d5dbe5; padding: 6px; margin-bottom: 12px; max-width: 360px; }
.infobox th { text-align: left; }
.thumb, figure { margin-bottom: 12px; }
.thumbcaption, figcaption { font-size: 14px; color: #555; }
.reference, sup { font-size: 11px; }
.reflist, .references, .mw-references-wrap { font-size: 14px; }
h1.wiki-title { font-size: 30px; margin-bottom: 4px; }
.wiki-src { font-size: 13px; color: #667085; margin-bottom: 16px; }
.search-hit { margin-bottom: 14px; }
.search-hit a { font-size: 18px; }
.searchmatch { font-weight: bold; }
`;

function wikiMatch(u) {
  const m = /^([a-z-]+)\.(?:m\.)?wikipedia\.org$/.exec(u.hostname);
  return m ? m[1] : null;
}

async function wikiArticle(lang, title) {
  const api = `https://${lang}.wikipedia.org/w/api.php?action=parse&page=${encodeURIComponent(title)}` +
    `&prop=text|displaytitle&format=json&formatversion=2&redirects=1&disableeditsection=1&origin=*`;
  const res = await withTimeout(fetch(api, { credentials: "omit" }), TIMEOUT_MS);
  const data = await res.json();
  if (data.error) {
    return { status: 404, body: `<html><head><title>${escapeHtml(title)}</title></head><body><h1>${escapeHtml(title.replace(/_/g, " "))}</h1><p>Wikipediassa ei ole tämän nimistä artikkelia.</p><p><a href="/w/index.php?search=${encodeURIComponent(title)}">Hae sanaa ${escapeHtml(title.replace(/_/g, " "))}</a></p></body></html>` };
  }
  const p = data.parse;
  const plainTitle = p.title;
  const html = `<!doctype html><html><head><title>${escapeHtml(plainTitle)} – Wikipedia</title><style>${WIKI_CSS}</style></head>` +
    `<body><h1 class="wiki-title">${p.displaytitle || escapeHtml(plainTitle)}</h1>` +
    `<div class="wiki-src">Wikipedia, vapaa tietosanakirja</div>${p.text}</body></html>`;
  return { status: 200, finalUrl: `https://${lang}.wikipedia.org/wiki/${encodeURIComponent(plainTitle.replace(/ /g, "_"))}`, body: html };
}

async function wikiSearch(lang, q) {
  const api = `https://${lang}.wikipedia.org/w/api.php?action=query&list=search&srsearch=${encodeURIComponent(q)}` +
    `&srlimit=20&format=json&formatversion=2&origin=*`;
  const res = await withTimeout(fetch(api, { credentials: "omit" }), TIMEOUT_MS);
  const data = await res.json();
  const hits = (data.query && data.query.search) || [];
  // an exact title is what someone typing a word into the address bar wants
  const exact = hits.find((h) => h.title.toLowerCase() === q.trim().toLowerCase());
  if (exact) return wikiArticle(lang, exact.title);
  let body = `<h1>Haku: ${escapeHtml(q)}</h1>`;
  if (!hits.length) body += `<p>Ei tuloksia.</p>`;
  for (const h of hits) {
    body += `<div class="search-hit"><a href="/wiki/${encodeURIComponent(h.title.replace(/ /g, "_"))}">${escapeHtml(h.title)}</a>` +
      `<div>${h.snippet || ""}…</div></div>`;
  }
  return {
    status: 200,
    finalUrl: `https://${lang}.wikipedia.org/w/index.php?search=${encodeURIComponent(q)}`,
    body: `<!doctype html><html><head><title>Haku: ${escapeHtml(q)}</title><style>${WIKI_CSS}</style></head><body>${body}</body></html>`,
  };
}

async function wikiAdapter(url) {
  const u = new URL(url);
  const lang = wikiMatch(u);
  if (!lang) return null;
  if (u.pathname.startsWith("/wiki/")) {
    return wikiArticle(lang, decodeURIComponent(u.pathname.slice(6)));
  }
  if (u.pathname === "/w/index.php" && u.searchParams.get("search") !== null) {
    return wikiSearch(lang, u.searchParams.get("search"));
  }
  if (u.pathname === "/" || u.pathname === "") {
    return wikiArticle(lang, lang === "fi" ? "Wikipedia:Etusivu" : "Main_Page");
  }
  return null;
}

// --- the fetcher ------------------------------------------------------------

export async function detectProxy(base = "./") {
  try {
    const res = await withTimeout(fetch(base + "__proxy/ping", { cache: "no-store" }), 1500);
    if (res.ok && (await res.text()).trim() === "evg-browser-proxy") return base + "__proxy/fetch?url=";
  } catch (e) { /* no proxy: a static host */ }
  return null;
}

export function createFetcher({ proxy = null } = {}) {
  async function viaProxy(url) {
    const res = await withTimeout(fetch(proxy + encodeURIComponent(url), { cache: "no-store" }), TIMEOUT_MS);
    const body = await res.text();
    return {
      status: res.status,
      finalUrl: res.headers.get("x-final-url") || url,
      contentType: res.headers.get("content-type") || "",
      body,
    };
  }

  async function direct(url) {
    const res = await withTimeout(fetch(url, { credentials: "omit", redirect: "follow" }), TIMEOUT_MS);
    return { status: res.status, finalUrl: res.url || url, contentType: res.headers.get("content-type") || "", body: await res.text() };
  }

  return async function fetchText(url, kind) {
    try {
      const sameOrigin = typeof location !== "undefined" && url.startsWith(location.origin + "/");
      if (proxy && !sameOrigin) return await viaProxy(url);
      if (kind === "page" && !sameOrigin) {
        const adapted = await wikiAdapter(url);
        if (adapted) return { contentType: "text/html", finalUrl: url, ...adapted };
      }
      return await direct(url);
    } catch (e) {
      const host = (() => { try { return new URL(url).host; } catch (_) { return url; } })();
      const why = String((e && e.message) || e);
      return {
        status: 0,
        finalUrl: "",
        contentType: "",
        body: proxy
          ? `Sivun ${host} haku epäonnistui: ${why}`
          : `Sivusto ${host} ei salli sen lukemista toiselta verkkosivulta (CORS), joten tämä verkkosivulla toimiva demo ei saa sitä auki. ` +
            `Wikipedia toimii, koska sillä on avoin rajapinta. Kaikki sivut toimivat, kun selaimen ajaa paikallisesti: node tools/serve.mjs. ` +
            `(${why})`,
      };
    }
  };
}
