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
 * qjs-bench --shell provides print, printErr, readFile, read, loadString and
 * runString. This adds `load`, and a second root for reads: a path the
 * submodule does not have (an inflated .z resource) is read from the
 * --js3-assets directory. Every realm runString makes gets the same functions,
 * bound to that realm. */
(function () {
  const g = globalThis;
  const rawRead = g.read;
  let assets = null;
  const rest = [];
  for (const a of g.arguments || []) {
    if (a.startsWith("--js3-assets=")) assets = a.slice("--js3-assets=".length);
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
    const rawRunString = realm.runString;
    realm.read = read;
    realm.readFile = readFile;
    realm.load = (path) => loadString(readFile(path), String(path));
    realm.runString = (src) => equip(rawRunString(src));
    return realm;
  }
  equip(g);
})();

load("./cli.js");
