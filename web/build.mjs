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
 *   evg_realm.js     the script realm (src/js/JsRealm.rgr), ComponentEngine
 *                    included; loaded into a Web Worker only when a page has
 *                    scripts, so a page without any never downloads it
 *
 * The Ranger checkout is RANGER_DIR, else ../Ranger next to this repository.
 */
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "..");
const RANGER = path.resolve(process.env.RANGER_DIR || path.join(ROOT, "..", "Ranger"));
const argv = process.argv.slice(2);
const outFlag = argv.indexOf("--out");
const OUT = outFlag >= 0 ? path.resolve(argv[outFlag + 1]) : path.join(HERE, "dist");
const STAGE = path.join(ROOT, "build", "web-stage");
const withRealm = !argv.includes("--no-realm");

if (!fs.existsSync(path.join(RANGER, "dist", "rgrc.js"))) {
  console.error(`Ranger compiler not found at ${RANGER}/dist/rgrc.js — set RANGER_DIR or clone Ranger next to this repository.`);
  process.exit(1);
}

fs.mkdirSync(STAGE, { recursive: true });
fs.mkdirSync(OUT, { recursive: true });

// Ranger -> one script, checked to load without Node's `require`, wrapped so
// only the names in `exportsJs` reach the global scope.
function bundle(entry, outName, globalName, exportsJs, probe) {
  let log = "";
  try {
    log = execFileSync(
      process.execPath,
      ["--stack-size=8000", path.join(RANGER, "dist", "rgrc.js"), "-es6", entry, `-d=${path.relative(ROOT, STAGE)}`, `-o=${outName}`, "-nodecli"],
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
  const scoped = `// GENERATED from ${entry} by web/build.mjs — do not edit.\n(function () {\n${code}\n;globalThis.${globalName} = ${exportsJs};\n})();\n`;
  fs.writeFileSync(path.join(OUT, outName), scoped);
  return scoped.length;
}

const sizes = [];
sizes.push(bundle("src/BrowserApp.rgr", "evg_browser.js", "EvgBrowser",
  "{ BrowserHost: BrowserHost, BrowserApp: BrowserApp, EVGHostTextMeasurer: EVGHostTextMeasurer, EVGDefaultMeasurer: EVGDefaultMeasurer }",
  "typeof BrowserHost + '|' + typeof EVGHostTextMeasurer"));
if (withRealm) {
  sizes.push(bundle("src/js/JsRealm.rgr", "evg_realm.js", "EvgRealm", "{ JsRealm: JsRealm }", "typeof JsRealm"));
}

for (const f of ["index.html", "main.js", "fetchers.js", "style.css", "realm-worker.js"]) {
  fs.copyFileSync(path.join(HERE, f), path.join(OUT, f));
}
fs.copyFileSync(path.join(RANGER, "lib/evg/html/evg-html.js"), path.join(OUT, "evg-html.js"));
fs.copyFileSync(path.join(RANGER, "lib/evg/gl/evg-measure.js"), path.join(OUT, "evg-measure.js"));
fs.cpSync(path.join(HERE, "samples"), path.join(OUT, "samples"), { recursive: true });
fs.writeFileSync(path.join(OUT, ".nojekyll"), "");
console.log(`Wrote ${path.relative(process.cwd(), OUT) || "."} (${sizes.map((n) => (n / 1024).toFixed(0) + " KB").join(" + ")})`);
