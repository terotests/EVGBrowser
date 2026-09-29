// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Where the Ranger compiler and the Ranger packages this project uses are.
//
// The compiler is the npm package `ranger-compiler` (a devDependency); the
// packages (evg, image, zip, componentengine) are named in ranger.json,
// pinned in ranger.lock, and `npm install` fetches them into vendor/ranger
// (`rgrc install -vendor`). RANGER_DIR, if set, points at a Ranger checkout
// whose compiler is used instead, for trying out a compiler change.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

export const RGRC = process.env.RANGER_DIR
  ? path.resolve(process.env.RANGER_DIR, "dist", "rgrc.js")
  : path.join(ROOT, "node_modules", "ranger-compiler", "dist", "rgrc.js");

/** The directory of a Ranger package from ranger.json, as `npm install` vendored it. */
export function rangerPackage(name) {
  return path.join(ROOT, "vendor", "ranger", name);
}

export function requireToolchain() {
  if (!fs.existsSync(RGRC)) {
    console.error(`Ranger compiler not found at ${RGRC} — run npm install.`);
    process.exit(1);
  }
  if (!fs.existsSync(path.join(rangerPackage("evg"), "ranger.json"))) {
    console.error("Ranger packages not found in vendor/ranger — run npm install (or npx rgrc install -vendor).");
    process.exit(1);
  }
}

// `node tools/ranger.mjs rgrc` prints the compiler's path, for shell scripts
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  if (process.argv[2] === "rgrc") {
    requireToolchain();
    console.log(RGRC);
  }
}
