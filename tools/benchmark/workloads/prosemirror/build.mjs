#!/usr/bin/env node
/*
 * Build tools/benchmark/workloads/95-prosemirror.js from the vendored
 * ProseMirror CommonJS bundles in ./vendor plus the workload logic in ./app.js.
 *
 * The benchmark harness (qjs-bench / Hermes) evaluates a workload file as a
 * single global script with no module loader, so the three CJS packages are
 * wrapped in a tiny `__pmRequire` shim and concatenated ahead of app.js.
 *
 * Regenerate with:  node tools/benchmark/workloads/prosemirror/build.mjs
 */
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, "..", "95-prosemirror.js");

const MODULES = [
  ["orderedmap", "vendor/orderedmap.cjs"],
  ["prosemirror-model", "vendor/prosemirror-model.cjs"],
  ["prosemirror-schema-basic", "vendor/prosemirror-schema-basic.cjs"],
];

function read(rel) {
  return readFileSync(join(HERE, rel), "utf8");
}

const out = [];
out.push("/*");
out.push(" * ProseMirror Node.fromJSON workload -- GENERATED FILE, DO NOT EDIT.");
out.push(" *");
out.push(" * Regenerate with:");
out.push(" *   node tools/benchmark/workloads/prosemirror/build.mjs");
out.push(" *");
out.push(" * Sources: ./app.js plus the vendored packages in ./vendor (see");
out.push(" * ./README.md for versions and provenance).");
out.push(" */");
out.push("");
out.push("var __pmModules = {}, __pmCache = {};");
out.push("function __pmDefine(name, fn) { __pmModules[name] = fn; }");
out.push("function __pmRequire(name) {");
out.push("  var c = __pmCache[name];");
out.push("  if (c) return c.exports;");
out.push("  var m = { exports: {} };");
out.push("  __pmCache[name] = m;");
out.push("  __pmModules[name](m, m.exports, __pmRequire);");
out.push("  return m.exports;");
out.push("}");
out.push("");

for (const [name, rel] of MODULES) {
  out.push(`__pmDefine(${JSON.stringify(name)}, function (module, exports, require) {`);
  out.push(read(rel));
  out.push("});");
  out.push("");
}

out.push("var OrderedMap = __pmRequire('orderedmap');");
out.push("var PM = __pmRequire('prosemirror-model');");
out.push("var schemaBasic = __pmRequire('prosemirror-schema-basic');");
out.push("");
out.push(read("app.js"));

writeFileSync(OUT, out.join("\n"));
const bytes = out.join("\n").length;
console.log(`wrote ${OUT} (${bytes} bytes)`);
