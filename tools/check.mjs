#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Compile the headless checks with the Ranger compiler and run them.
//
//   node tools/check.mjs          JavaScript (Node), including the script realm
//   node tools/check.mjs --fast   without the script realm
//   node tools/check.mjs --cpp    the same checks through C++ (g++ or clang++),
//                                 which is the target the SDL host builds on
//
// The compiler and the Ranger packages come from npm install (tools/ranger.mjs).
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { ROOT, RGRC, requireToolchain } from "./ranger.mjs";

requireToolchain();
const cpp = process.argv.includes("--cpp");

// (the checks with the realm and RangerDOM's view in them take more than
// node's default heap to compile; a compile that dies must fail the check,
// not leave the last build's output to be run)
function compile(args) {
  const d = args.find((a) => a.startsWith("-d=")).slice(3);
  const o = args.find((a) => a.startsWith("-o=")).slice(3);
  const out = path.join(ROOT, d, o);
  fs.rmSync(out, { force: true });
  let log = "", failed = false;
  try {
    log = execFileSync(process.execPath, ["--max-old-space-size=8192", "--stack-size=8000", RGRC, ...args], { cwd: ROOT, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"], maxBuffer: 256 * 1024 * 1024 });
  } catch (e) {
    log = String(e.stdout || "") + String(e.stderr || "");
    failed = true;
  }
  if (failed || log.includes("Compilation FAILED") || log.includes("[FAIL]") || !fs.existsSync(out)) {
    process.stderr.write(log.split("\n").filter((l) => !l.includes("api docs")).slice(-60).join("\n") + "\n");
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
  if (!process.argv.includes("--fast")) {
    compile(["-l=cpp", "tests/RealmCheck.rgr", "-d=build/cpp", "-o=realm_check.cpp"]);
    execFileSync(cxx, ["-std=c++17", "-O1", "-w", "build/cpp/realm_check.cpp", "-o", "build/cpp/realm_check"], { cwd: ROOT, stdio: "inherit" });
    run(path.join(ROOT, "build/cpp/realm_check"), []);
  }
} else {
  compile(["-es6", "tests/ParseCheck.rgr", "-d=build", "-o=parse_check.js", "-nodecli"]);
  compile(["-es6", "tests/BrowserCheck.rgr", "-d=build", "-o=browser_check.js", "-nodecli"]);
  run(process.execPath, [path.join(ROOT, "build/browser_check.js")]);
  if (!process.argv.includes("--fast")) {
    // the script realm: RangerDOM on CEr, with React 18 from RangerDOM's tests
    compile(["-es6", "tests/RealmCheck.rgr", "-d=build", "-o=realm_check.cjs", "-nodecli"]);
    run(process.execPath, [path.join(ROOT, "build/realm_check.cjs")]);
  }
}
