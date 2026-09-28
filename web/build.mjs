#!/usr/bin/env node
/**
 * Build the browser-in-browser page.
 *
 *   node web/build.mjs                 # -> web/dist/
 *   node web/build.mjs --out site/     # somewhere else (the Pages build)
 *
 * Compiles src/BrowserApp.rgr — the whole browser, EVG included — to one
 * JavaScript bundle with the Ranger compiler, and copies the page, its host
 * script and EVG's SVG painter and canvas text measurer beside it. The Ranger
 * checkout is found from RANGER_DIR, else ../Ranger next to this repository.
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

if (!fs.existsSync(path.join(RANGER, "dist", "rgrc.js"))) {
  console.error(`Ranger compiler not found at ${RANGER}/dist/rgrc.js — set RANGER_DIR or clone Ranger next to this repository.`);
  process.exit(1);
}

fs.mkdirSync(STAGE, { recursive: true });
fs.mkdirSync(OUT, { recursive: true });

let log = "";
try {
  log = execFileSync(
    process.execPath,
    [path.join(RANGER, "dist", "rgrc.js"), "-es6", "src/BrowserApp.rgr", `-d=${path.relative(ROOT, STAGE)}`, "-o=evg_browser.js", "-nodecli"],
    { cwd: ROOT, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }
  );
} catch (e) {
  log = String(e.stdout || "") + String(e.stderr || "");
}
if (log.includes("Compilation FAILED") || log.includes("[FAIL]")) {
  process.stderr.write(log + "\n");
  process.exit(1);
}

const rawPath = path.join(STAGE, "evg_browser.js");
const bundle = fs.readFileSync(rawPath, "utf8").replace(/^#![^\n]*\n/, "");

// Load it the way the browser will, with no `require`, so a bundle that only
// works on Node fails here and not on the page.
{
  const previous = globalThis.require;
  globalThis.require = undefined;
  const found = (0, eval)(bundle + "; typeof BrowserHost + '|' + typeof EVGHostTextMeasurer");
  globalThis.require = previous;
  if (found !== "function|function") throw new Error("bundle is missing its browser exports (" + found + ")");
}

const scoped =
  "// GENERATED from src/BrowserApp.rgr by web/build.mjs — do not edit.\n" +
  "(function () {\n" + bundle +
  "\n;globalThis.EvgBrowser = { BrowserHost: BrowserHost, BrowserApp: BrowserApp," +
  " EVGHostTextMeasurer: EVGHostTextMeasurer, EVGDefaultMeasurer: EVGDefaultMeasurer };" +
  "\n})();\n";

fs.writeFileSync(path.join(OUT, "evg_browser.js"), scoped);
for (const f of ["index.html", "main.js", "fetchers.js", "style.css"]) {
  fs.copyFileSync(path.join(HERE, f), path.join(OUT, f));
}
fs.copyFileSync(path.join(RANGER, "lib/evg/html/evg-html.js"), path.join(OUT, "evg-html.js"));
fs.copyFileSync(path.join(RANGER, "lib/evg/gl/evg-measure.js"), path.join(OUT, "evg-measure.js"));
fs.cpSync(path.join(HERE, "samples"), path.join(OUT, "samples"), { recursive: true });
fs.writeFileSync(path.join(OUT, ".nojekyll"), "");
console.log(`Wrote ${path.relative(process.cwd(), OUT) || "."} (${(scoped.length / 1024).toFixed(0)} KB bundle)`);
