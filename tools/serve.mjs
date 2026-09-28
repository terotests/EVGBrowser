#!/usr/bin/env node
// SPDX-License-Identifier: MIT
//
// Serve the built page locally, with a fetch proxy beside it.
//
//   node web/build.mjs && node tools/serve.mjs      # http://localhost:8040/
//
// A page in a browser tab may only read another site when that site allows
// it (CORS), and most do not — yle.fi does not. The page looks for
// `__proxy/ping` when it starts; this answers it, and `__proxy/fetch?url=`
// then fetches pages and stylesheets server-side.
//
// It is NOT an open proxy: it fetches only from the hosts on the allowlist
// (the same defaults as src/SafetyPolicy.rgr, or EVG_ALLOW="a.fi,b.org"),
// only GET, only text, without cookies, and it listens on localhost.

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DIST = path.resolve(process.argv[2] || path.join(HERE, "..", "web", "dist"));
const PORT = Number(process.env.PORT || 8040);
const ALLOW = (process.env.EVG_ALLOW ||
  "yle.fi,wikipedia.org,wikimedia.org,vikidia.org,kirjastot.fi,luontoportti.com,tiedekeskus.fi,heureka.fi,korkeasaari.fi,nasa.gov,esa.int,bbc.co.uk,natgeokids.com,example.com,info.cern.ch,terotests.github.io")
  .split(",").map((s) => s.trim().toLowerCase()).filter(Boolean);
const MAX_BYTES = 5_000_000;

const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".mjs": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8", ".json": "application/json", ".png": "image/png", ".svg": "image/svg+xml" };

function allowed(host) {
  host = host.toLowerCase();
  return ALLOW.some((a) => host === a || host.endsWith("." + a));
}

async function proxy(req, res, target) {
  let u;
  try { u = new URL(target); } catch (e) { res.writeHead(400); return res.end("bad url"); }
  if (!/^https?:$/.test(u.protocol) || !allowed(u.hostname)) {
    res.writeHead(403, { "content-type": "text/plain; charset=utf-8" });
    return res.end("not on the allowlist: " + u.hostname);
  }
  try {
    const r = await fetch(u, {
      redirect: "follow",
      headers: { "user-agent": "EVG-Browser/0.1 (+https://github.com/terotests/EVGBrowser)", "accept": "text/html,text/css,*/*;q=0.5", "accept-language": "fi,en;q=0.7" },
    });
    const final = new URL(r.url || u.href);
    if (!allowed(final.hostname)) {
      res.writeHead(403, { "content-type": "text/plain; charset=utf-8", "x-final-url": final.href });
      return res.end("redirected off the allowlist: " + final.hostname);
    }
    const type = r.headers.get("content-type") || "";
    if (!/text|html|css|xml|json/.test(type)) {
      res.writeHead(415, { "content-type": "text/plain" });
      return res.end("not text: " + type);
    }
    const buf = Buffer.from(await r.arrayBuffer());
    const body = buf.length > MAX_BYTES ? buf.subarray(0, MAX_BYTES) : buf;
    res.writeHead(r.status, { "content-type": type, "x-final-url": final.href, "cache-control": "no-store", "access-control-expose-headers": "x-final-url" });
    res.end(body);
  } catch (e) {
    res.writeHead(502, { "content-type": "text/plain; charset=utf-8" });
    res.end("fetch failed: " + e.message);
  }
}

http.createServer((req, res) => {
  const url = new URL(req.url, "http://localhost");
  if (url.pathname.endsWith("/__proxy/ping")) {
    res.writeHead(200, { "content-type": "text/plain" });
    return res.end("evg-browser-proxy");
  }
  if (url.pathname.endsWith("/__proxy/fetch")) return proxy(req, res, url.searchParams.get("url") || "");
  let p = path.normalize(decodeURIComponent(url.pathname)).replace(/^([/\\])+/, "");
  if (!p || p.endsWith("/")) p += "index.html";
  const file = path.join(DIST, p);
  if (!file.startsWith(DIST) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404);
    return res.end("not found");
  }
  res.writeHead(200, { "content-type": TYPES[path.extname(file)] || "application/octet-stream" });
  fs.createReadStream(file).pipe(res);
}).listen(PORT, "127.0.0.1", () => console.log(`EVG Browser: http://localhost:${PORT}/  (serving ${DIST})`));
