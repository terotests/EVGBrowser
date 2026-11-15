#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later
/**
 * Build the browser-in-browser page.
 *
 *   node web/build.mjs                 # -> web/dist/
 *   node web/build.mjs --out site/     # somewhere else (the Pages build)
 *   node web/build.mjs --no-realm      # skip the script realm (faster)
 *
 * Two bundles, both compiled from Ranger with the Ranger compiler:
 *
 *   evg_browser.js   the browser (src/BrowserApp.rgr), EVG included
 *   evg_realm.js     the script realm (src/js/PageRealm.rgr), RangerDOM and
 *                    CEr included; loaded into a Web Worker only when a page
 *                    has scripts, so a page without any never downloads it
 *
 * The compiler and the Ranger packages come from npm install (tools/ranger.mjs).
 */
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { ROOT, RGRC, rangerPackage, requireToolchain } from "../tools/ranger.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const outFlag = argv.indexOf("--out");
const OUT = outFlag >= 0 ? path.resolve(argv[outFlag + 1]) : path.join(HERE, "dist");
const STAGE = path.join(ROOT, "build", "web-stage");
const withRealm = !argv.includes("--no-realm");

requireToolchain();

fs.mkdirSync(STAGE, { recursive: true });
fs.mkdirSync(OUT, { recursive: true });

// Ranger's JavaScript for a Rust `s.as_bytes()[i]` encodes the whole string
// again on each read (r_cb_enc.encode), so CEr's lexer is quadratic in a
// script's length: RangerDOM's webapi.js alone took over 10 s to load in the
// realm. Until the compiler keeps the bytes, the encoder keeps its last
// sixteen long answers and the short ones (strings do not change; the
// answers are only read). The same patch as RangerDOM's playground build.
const ENCODER_CACHE = `if (typeof r_cb_enc !== "undefined" && r_cb_enc && typeof r_cb_enc.encode === "function") {
  const raw = r_cb_enc.encode.bind(r_cb_enc), kept = new Map(), small = new Map();
  r_cb_enc.encode = function (s) {
    if (s.length <= 4) { let w = small.get(s); if (w === undefined) { w = raw(s); if (small.size < 4096) small.set(s, w); } return w; }
    if (s.length < 256) return raw(s);
    let v = kept.get(s);
    if (v === undefined) { v = raw(s); if (kept.size >= 16) kept.delete(kept.keys().next().value); kept.set(s, v); }
    return v;
  };
}`;

// Ranger -> one script, checked to load without Node's `require`, wrapped so
// only the names in `exportsJs` reach the global scope.
function bundle(entry, outName, globalName, exportsJs, probe) {
  let log = "";
  try {
    log = execFileSync(
      process.execPath,
      ["--max-old-space-size=8192", "--stack-size=8000", RGRC, "-es6", entry, `-d=${path.relative(ROOT, STAGE)}`, `-o=${outName}`, "-nodecli"],
      { cwd: ROOT, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"], maxBuffer: 64 * 1024 * 1024 }
    );
  } catch (e) {
    log = String(e.stdout || "") + String(e.stderr || "");
  }
  if (log.includes("Compilation FAILED") || log.includes("[FAIL]")) {
    process.stderr.write(log + "\n");
    process.exit(1);
  }
  const code = fs.readFileSync(path.join(STAGE, outName), "utf8").replace(/^#![^\n]*\n/, "");
  const previous = globalThis.require;
  globalThis.require = undefined;
  const found = (0, eval)(code + ";" + probe);
  globalThis.require = previous;
  if (!/^function(\|function)*$/.test(found)) throw new Error(`${outName} is missing its browser exports (${found})`);
  const scoped = `// GENERATED from ${entry} by web/build.mjs — do not edit.\n(function () {\n${code}\n;${ENCODER_CACHE}\nglobalThis.${globalName} = ${exportsJs};\n})();\n`;
  fs.writeFileSync(path.join(OUT, outName), scoped);
  return scoped.length;
}

const sizes = [];
sizes.push(bundle("src/BrowserApp.rgr", "evg_browser.js", "EvgBrowser",
  "{ BrowserHost: BrowserHost, BrowserApp: BrowserApp, EVGHostTextMeasurer: EVGHostTextMeasurer, EVGDefaultMeasurer: EVGDefaultMeasurer }",
  "typeof BrowserHost + '|' + typeof EVGHostTextMeasurer"));
if (withRealm) {
  sizes.push(bundle("src/js/PageRealm.rgr", "evg_realm.js", "EvgRealm", "{ PageRealm: PageRealm }", "typeof PageRealm"));
}

for (const f of ["index.html", "main.js", "fetchers.js", "style.css", "realm-worker.js"]) {
  fs.copyFileSync(path.join(HERE, f), path.join(OUT, f));
}
fs.copyFileSync(path.join(rangerPackage("evg"), "html/evg-html.js"), path.join(OUT, "evg-html.js"));
fs.copyFileSync(path.join(rangerPackage("evg"), "gl/evg-measure.js"), path.join(OUT, "evg-measure.js"));
fs.cpSync(path.join(HERE, "samples"), path.join(OUT, "samples"), { recursive: true });
fs.writeFileSync(path.join(OUT, ".nojekyll"), "");
console.log(`Wrote ${path.relative(process.cwd(), OUT) || "."} (${sizes.map((n) => (n / 1024).toFixed(0) + " KB").join(" + ")})`);
