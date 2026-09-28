#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later
// src/js/prelude.js -> src/js/JsPrelude.rgr, the DOM prelude as a Ranger string.
//   node tools/gen-prelude.mjs          write it
//   node tools/gen-prelude.mjs --check  fail if it is out of date
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const src = fs.readFileSync(path.join(ROOT, "src/js/prelude.js"), "utf8");
const lines = src.split("\n");
const lit = (s) => '"' + s.replace(/\\/g, "\\\\").replace(/"/g, '\\"') + '\\n"';
let out = "; SPDX-License-Identifier: AGPL-3.0-or-later\n";
out += "; GENERATED from src/js/prelude.js by tools/gen-prelude.mjs — do not edit.\n\n";
out += "class JsPrelude {\n    sfn source:string () {\n        def s:string \"\"\n";
for (const l of lines) {
  if (l.trim().startsWith("//") || l.trim() === "") continue;
  out += "        s = (s + " + lit(l) + ")\n";
}
out += "        return s\n    }\n}\n";
const dest = path.join(ROOT, "src/js/JsPrelude.rgr");
if (process.argv.includes("--check")) {
  if (!fs.existsSync(dest) || fs.readFileSync(dest, "utf8") !== out) {
    console.error("src/js/JsPrelude.rgr is out of date: run node tools/gen-prelude.mjs");
    process.exit(1);
  }
} else {
  fs.writeFileSync(dest, out);
  console.log("wrote src/js/JsPrelude.rgr");
}
