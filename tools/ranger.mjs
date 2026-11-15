// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Where the Ranger compiler and the Ranger packages this project uses are.
//
// The compiler is the Ranger repository at the commit ranger-compiler.json
// names, fetched into .deps/ranger (dist/, compiler/, lib/ only) the first
// time it is needed: RangerDOM's view (DomView, DomPage) is Rust that
// Ranger reads, which the npm release of the compiler cannot yet.
// RANGER_DIR, if set, points at a Ranger checkout whose compiler is used
// instead, for trying out a compiler change.
//
// The packages (evg, image, rangerdom and what they bring) are named in
// ranger.json, pinned in ranger.lock, and `npm install` fetches them into
// vendor/ranger (`rgrc install -vendor`, the npm package ranger-compiler).
// RangerDOM's crates read EVG as ../../../lib/evg and the JSON crate as
// ../domjson, as they do in a Ranger checkout: two links in vendor/ranger
// give them that layout (linkRangerDom).
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

const PIN = JSON.parse(fs.readFileSync(path.join(ROOT, "ranger-compiler.json"), "utf8"));
const RANGER = process.env.RANGER_DIR ? path.resolve(process.env.RANGER_DIR) : path.join(ROOT, ".deps", "ranger");

export const RGRC = path.join(RANGER, "dist", "rgrc.js");

/** The directory of a Ranger package from ranger.json, as `npm install` vendored it. */
export function rangerPackage(name) {
  return path.join(ROOT, "vendor", "ranger", name);
}

// .deps/ranger at the pinned commit (sparse, no history)
function fetchCompiler() {
  if (process.env.RANGER_DIR) return;
  const git = (...a) => execFileSync("git", ["-C", RANGER, ...a], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).trim();
  if (fs.existsSync(path.join(RANGER, ".git"))) {
    let head = "";
    try { head = git("rev-parse", "HEAD"); } catch (_) {}
    if (head === PIN.rev && fs.existsSync(RGRC)) return;
  } else {
    fs.mkdirSync(RANGER, { recursive: true });
    git("init", "-q");
    git("remote", "add", "origin", PIN.git);
  }
  console.error(`==> Ranger compiler ${PIN.rev.slice(0, 7)} (.deps/ranger)`);
  git("sparse-checkout", "init", "--no-cone");
  git("sparse-checkout", "set", "--no-cone", ...PIN.sparse.map((p) => "/" + p + "/"));
  git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", PIN.rev);
  git("checkout", "-q", "-f", "--detach", "FETCH_HEAD");
  // dist/rgrc.js is CommonJS; this repository's package.json says "module"
  fs.writeFileSync(path.join(RANGER, "package.json"), '{ "type": "commonjs" }\n');
}

// vendor/ranger/lib/evg -> ../evg and vendor/ranger/rangerdom/domjson -> json
function link(target, at) {
  try {
    if (fs.readlinkSync(at) === target) return;
    fs.rmSync(at, { force: true });
  } catch (_) {
    if (fs.existsSync(at)) return;
  }
  fs.mkdirSync(path.dirname(at), { recursive: true });
  fs.symlinkSync(target, at, "dir");
}

export function linkRangerDom() {
  link("../evg", path.join(ROOT, "vendor", "ranger", "lib", "evg"));
  link("json", path.join(rangerPackage("rangerdom"), "domjson"));
}

export function requireToolchain() {
  if (!fs.existsSync(path.join(rangerPackage("evg"), "ranger.json")) || !fs.existsSync(path.join(rangerPackage("rangerdom"), "view"))) {
    console.error("Ranger packages not found in vendor/ranger — run npm install (or npx rgrc install -vendor).");
    process.exit(1);
  }
  try {
    fetchCompiler();
  } catch (e) {
    console.error(`Could not fetch the Ranger compiler (${PIN.git} ${PIN.rev}): ${e.message}`);
    process.exit(1);
  }
  if (!fs.existsSync(RGRC)) {
    console.error(`Ranger compiler not found at ${RGRC}.`);
    process.exit(1);
  }
  linkRangerDom();
}

// `node tools/ranger.mjs rgrc` prints the compiler's path, for shell scripts
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  if (process.argv[2] === "rgrc") {
    requireToolchain();
    console.log(RGRC);
  }
}
