/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* JetStream's cli.js on node and bun. Run from the JetStream directory:
 *
 *   cd tools/benchmark/jetstream3/JetStream
 *   node ../shell-node.cjs -- --js3-assets=<dir> --no-prefetch --test=Air
 *
 * Gives the driver the shell it expects (print, printErr, readFile, read,
 * load, runString, `arguments`). Scripts run through node:vm, so each file is
 * its own global script as in a JS shell, and runString makes a fresh context
 * per workload. A path the submodule does not have (an inflated .z resource)
 * is read from --js3-assets. With --js3-root=<dir> the files are read from
 * there instead of the working directory (a compiled bun executable). */
"use strict";
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

let assets = null;
let root = process.cwd();
const argv = process.argv.slice(2);
const dd = argv.indexOf("--");
const rest = [];
for (const a of dd >= 0 ? argv.slice(dd + 1) : argv) {
  if (a.startsWith("--js3-assets=")) assets = a.slice("--js3-assets=".length);
  else if (a.startsWith("--js3-root=")) root = a.slice("--js3-root=".length);
  else rest.push(a);
}

function resolvePath(p) {
  const rel = String(p).replace(/^\.\//, "");
  const a = path.resolve(root, rel);
  if (assets && !fs.existsSync(a)) return path.join(assets, rel);
  return a;
}

function read(p, mode) {
  const buf = fs.readFileSync(resolvePath(p));
  if (mode === "binary")
    return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  return buf.toString("utf8");
}
const readFile = (p) => read(p);
const print = (...a) => console.log(a.join(" "));
const printErr = (...a) => console.error(a.join(" "));

function equip(g, evaluate) {
  g.print = print;
  g.printErr = printErr;
  g.read = read;
  g.readFile = readFile;
  g.loadString = (src, name) => evaluate(String(src), name || "<loadString>");
  g.load = (p) => evaluate(readFile(p), String(p));
  g.runString = runString;
  g.setTimeout = setTimeout;
  g.clearTimeout = clearTimeout;
  return g;
}

function runString(src) {
  const ctx = vm.createContext({});
  const g = vm.runInContext("globalThis", ctx);
  equip(g, (s, filename) => vm.runInContext(s, ctx, { filename }));
  if (src) g.loadString(src);
  return g;
}

equip(globalThis, (s, filename) => vm.runInThisContext(s, { filename }));
globalThis.arguments = rest;
globalThis.load("./cli.js");
