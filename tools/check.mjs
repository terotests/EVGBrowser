#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Compile the headless checks with the Ranger compiler and run them.
//
//   node tools/check.mjs          JavaScript (Node), including the script realm
//   node tools/check.mjs --fast   without the realm (it takes ~90 s to compile)
//   node tools/check.mjs --cpp    the same checks through C++ (g++ or clang++),
//                                 which is the target the SDL host builds on
//
// The Ranger checkout is RANGER_DIR, else ../Ranger beside this repository.
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const RANGER = path.resolve(process.env.RANGER_DIR || path.join(ROOT, "..", "Ranger"));
const RGRC = path.join(RANGER, "dist", "rgrc.js");
const cpp = process.argv.includes("--cpp");

function compile(args) {
  let log = "";
  try {
    log = execFileSync(process.execPath, [RGRC, ...args], { cwd: ROOT, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] });
  } catch (e) {
    log = String(e.stdout || "") + String(e.stderr || "");
  }
  if (log.includes("Compilation FAILED") || log.includes("[FAIL]")) {
    process.stderr.write(log + "\n");
    process.exit(1);
  }
}

function run(cmd, args) {
  const out = execFileSync(cmd, args, { cwd: ROOT, encoding: "utf8" });
  process.stdout.write(out);
  if (/failed [1-9]/.test(out) || /^FAIL/m.test(out)) process.exit(1);
}

fs.mkdirSync(path.join(ROOT, "build"), { recursive: true });
if (cpp) {
  compile(["-l=cpp", "tests/BrowserCheck.rgr", "-d=build/cpp", "-o=browser_check.cpp"]);
  const cxx = ["g++", "clang++"].find((c) => { try { execFileSync(c, ["--version"], { stdio: "ignore" }); return true; } catch (_) { return false; } });
  if (!cxx) { console.error("no C++ compiler found"); process.exit(1); }
  execFileSync(cxx, ["-std=c++17", "-O1", "-w", "build/cpp/browser_check.cpp", "-o", "build/cpp/browser_check"], { cwd: ROOT, stdio: "inherit" });
  run(path.join(ROOT, "build/cpp/browser_check"), []);
} else {
  compile(["-es6", "tests/ParseCheck.rgr", "-d=build", "-o=parse_check.js", "-nodecli"]);
  compile(["-es6", "tests/BrowserCheck.rgr", "-d=build", "-o=browser_check.js", "-nodecli"]);
  run(process.execPath, [path.join(ROOT, "build/browser_check.js")]);
  if (!process.argv.includes("--fast")) {
    // the script realm: ComponentEngine is large, so this one takes a minute and a half
    execFileSync(process.execPath, [path.join(ROOT, "tools/gen-prelude.mjs"), "--check"], { stdio: "inherit" });
    compile(["-es6", "tests/RealmCheck.rgr", "-d=build", "-o=realm_check.cjs", "-nodecli"]);
    run(process.execPath, [path.join(ROOT, "build/realm_check.cjs")]);
  }
}
