/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* JetStream's cli.js on qjs-bench --shell. Run from the JetStream directory:
 *
 *   cd tools/benchmark/jetstream3/JetStream
 *   qjs-bench --shell ../shell-qjs.js -- --js3-assets=<dir> --no-prefetch --test=Air
 *
 * qjs-bench --shell provides print, printErr, readFile, read, loadString,
 * loadBytecode and runString. This adds `load`, and a second root for reads: a
 * path the submodule does not have (an inflated .z resource) is read from the
 * --js3-assets directory. With --js3-bc=<manifest.json> (written by bench.mjs:
 * each workload script's load() name -> its qjsc bytecode), load() runs the
 * listed scripts from their bytecode, as an app runs precompiled code; the
 * driver, the preamble and anything else stay source. Every realm runString
 * makes gets the same functions, bound to that realm. */
(function () {
  const g = globalThis;
  const rawRead = g.read;
  let assets = null;
  let bytecode = null;
  const rest = [];
  for (const a of g.arguments || []) {
    if (a.startsWith("--js3-assets=")) assets = a.slice("--js3-assets=".length);
    else if (a.startsWith("--js3-bc=")) bytecode = JSON.parse(rawRead(a.slice("--js3-bc=".length)));
    else rest.push(a);
  }
  g.arguments = rest;

  function read(path, mode) {
    try {
      return rawRead(path, mode);
    } catch (e) {
      if (!assets) throw e;
      return rawRead(assets + "/" + String(path).replace(/^\.\//, ""), mode);
    }
  }
  const readFile = (path) => read(path);

  function equip(realm) {
    const loadString = realm.loadString;
    const loadBytecode = realm.loadBytecode;
    const rawRunString = realm.runString;
    realm.read = read;
    realm.readFile = readFile;
    realm.load = (path) => {
      const bc = bytecode && bytecode[String(path)];
      return bc ? loadBytecode(bc) : loadString(readFile(path), String(path));
    };
    realm.runString = (src) => equip(rawRunString(src));
    return realm;
  }
  equip(g);
})();

load("./cli.js");
