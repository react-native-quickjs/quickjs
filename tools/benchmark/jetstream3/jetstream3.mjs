/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* JetStream 3 for bench.mjs: the workload list, the asset preparation and the
 * command line that runs JetStream's own cli.js driver on an engine.
 *
 * The driver and the workloads run unmodified from the JetStream submodule. A
 * shell shim (shell-qjs.js for qjs-bench, shell-node.cjs for node and bun)
 * gives each engine what the driver expects of a JS shell: print/printErr,
 * load/read/readFile, `arguments` and runString (a fresh global per workload).
 * The driver runs without prefetching, so it reads the uncompressed name of
 * each .z resource; those are inflated once into build/assets, which the shims
 * search after the submodule. */

import {
  existsSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
  statSync,
} from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { inflateSync } from "node:zlib";
import vm from "node:vm";
import { createRequire } from "node:module";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";

export const HERE = dirname(fileURLToPath(import.meta.url));
export const JS3 = join(HERE, "JetStream");
export const BUILD = join(HERE, "build");
export const ASSETS = join(BUILD, "assets");

/* Tags of workloads that cannot run in a JS shell without WebAssembly or
 * Workers. Everything else tagged "js" is in the suite, except the workloads
 * below, which are tagged "js" but still need one of them. */
const EXCLUDED_TAGS = ["wasm", "workertests"];
const EXCLUDED_NAMES = {
  "source-map-wtb": "needs WebAssembly (source-map's wasm mappings parser)",
};

let _plan = null;

/* JetStreamDriver.js loaded into a node:vm context with a do-nothing shell,
 * without prefetching; `params` are extra driver parameters by their
 * JetStreamParams name (testIterationCount, testWorstCaseCount). */
function driverContext(params = {}) {
  const ctx = vm.createContext({});
  const run = (p) =>
    vm.runInContext(readFileSync(join(JS3, p), "utf8"), ctx, { filename: p });
  ctx.print = () => {};
  ctx.printErr = () => {};
  ctx.load = (p) => run(p);
  ctx.performance = { now: () => 0 };
  ctx.JetStreamParamsSource = new Map([
    ["prefetchResources", "false"],
    ...Object.entries(params).map(([k, v]) => [k, String(v)]),
  ]);
  run("./utils/shell-config.js");
  run("./utils/params.js");
  run("./JetStreamDriver.js");
  return ctx;
}

/* Evaluates JetStreamDriver.js in a node:vm context with a do-nothing shell
 * and returns every benchmark's plan as the driver itself computes it
 * (iterations, worst-case count, scoring class, files, preloads). */
export function plan() {
  if (_plan) return _plan;
  if (!existsSync(join(JS3, "JetStreamDriver.js")))
    throw new Error(
      "JetStream submodule missing: git submodule update --init tools/benchmark/jetstream3/JetStream",
    );
  const ctx = driverContext();
  const raw = vm.runInContext(
    `BENCHMARKS.map((b) => {
       const one = (x) => ({
         name: x.name,
         kind: x.constructor.name,
         tags: [...x.tags],
         iterations: x.iterations,
         worstCaseCount: x.worstCaseCount ?? null,
         files: x.files,
         preload: Object.fromEntries(x.preloadEntries),
       });
       const o = one(b);
       if (b.benchmarks) o.subs = b.benchmarks.map(one);
       return JSON.stringify(o);
     })`,
    ctx,
  );
  _plan = raw.map((s) => {
    const b = JSON.parse(s);
    const excluded = EXCLUDED_TAGS.filter((t) => b.tags.includes(t));
    const byName = EXCLUDED_NAMES[b.name] || null;
    b.included = b.tags.includes("js") && excluded.length === 0 && !byName;
    b.reason = !b.tags.includes("js")
      ? "not tagged js"
      : excluded.length
        ? `tagged ${excluded.join(",")}`
        : byName;
    b.default = b.tags.includes("default");
    return b;
  });
  return _plan;
}

export function workloads() {
  return plan().filter((b) => b.included);
}

/* Every file a workload's run reads, as the driver names them. */
function filesOf(b) {
  const out = [];
  for (const x of b.subs || [b]) {
    out.push(...x.files, ...Object.values(x.preload));
  }
  return out;
}

/* Inflates the .z resources the included workloads name into build/assets,
 * at the path the driver asks for (the name without .z). */
export function prepareAssets({ quiet = false } = {}) {
  let made = 0;
  for (const b of workloads()) {
    for (const f of filesOf(b)) {
      if (!f.endsWith(".z")) continue;
      const src = join(JS3, f);
      const dst = join(ASSETS, f.slice(0, -2));
      if (existsSync(dst) && statSync(dst).mtimeMs >= statSync(src).mtimeMs)
        continue;
      mkdirSync(dirname(dst), { recursive: true });
      writeFileSync(dst, inflateSync(readFileSync(src)));
      made++;
      if (!quiet) process.stderr.write(`[jetstream3] inflated ${f}\n`);
    }
  }
  return made;
}

/* The driver's flags for one run. `smoke` caps the iteration count (and the
 * worst-case count below it) for a correctness pass; scores from a smoke run
 * are not JetStream scores. */
/* Whether a workload (or a grouped row's member) is tagged internationalization: qjs-bench then runs with --intl,
 * which installs Intl (modules/intl, the host's en-US/UTC stub backend) in every shell realm. */
export function needsIntl(name) {
  for (const b of plan()) {
    if (b.name === name || (b.subs || []).some((x) => x.name === name))
      return b.tags.includes("internationalization");
  }
  return false;
}

export function driverArgs(name, { iterations = null, worstCase = null } = {}) {
  const a = ["--no-prefetch"];
  if (iterations != null) a.push(`--iteration-count=${iterations}`);
  if (worstCase != null) a.push(`--worst-case-count=${worstCase}`);
  a.push(`--test=${name}`);
  return a;
}

/* Parses the driver's console output for one workload: its overall score and
 * its sub-scores ("Air First-Score 123.45 pts"). */
export function parseOutput(name, text) {
  const esc = name.replace(/[-/\\^$*+?.()|[\]{}]/g, "\\$&");
  const sub = {};
  let score = null;
  for (const line of text.split("\n")) {
    const m = new RegExp(`^${esc} (\\S+)-Score\\s+([\\d.]+) pts`).exec(line);
    if (m) {
      if (m[1] === "Score") score = Number(m[2]);
      else sub[m[1]] = Number(m[2]);
      continue;
    }
    const s = new RegExp(`^${esc} Score\\s+([\\d.]+) pts`).exec(line);
    if (s) score = Number(s[1]);
  }
  const overall = /^Overall Score\s+([\d.]+) pts/m.exec(text);
  if (score == null && overall) score = Number(overall[1]);
  return { score, sub };
}

export const SHELL_QJS = join(HERE, "shell-qjs.js");
export const SHELL_NODE = join(HERE, "shell-node.cjs");

/* bun-aot: the node/bun shim compiled by bun 1.4.4's ahead-of-time mode, a
 * standalone executable with a bytecode cache (`bun build --compile
 * --bytecode`; released bun has no --aot flag). Only the shim is in that
 * bytecode: JetStream evaluates the driver and every workload from source
 * strings at run time (load/loadString into a fresh vm context), as in any
 * shell, so their code is compiled by the runtime as with plain bun. Rebuilt
 * when the shim is newer than the executable. */
export function buildBunAot() {
  const exe = join(BUILD, "bun-aot", "jetstream3-shell");
  if (existsSync(exe) && statSync(exe).mtimeMs >= statSync(SHELL_NODE).mtimeMs)
    return exe;
  mkdirSync(dirname(exe), { recursive: true });
  const bun = process.env.BUN_BIN || "bun";
  const r = spawnSync(
    bun,
    ["build", "--compile", "--bytecode", "--format=cjs", "--target=bun",
      SHELL_NODE, "--outfile", exe],
    { encoding: "utf8" },
  );
  if (r.status !== 0)
    throw new Error(`bun build --compile --bytecode failed: ${r.stderr || r.error}`);
  return exe;
}

export function shimArgs() {
  return [`--js3-assets=${resolve(ASSETS)}`];
}

/* ---- bun-canary-aot: one bundle per workload --------------------------------------------------
 * `bun build --compile --aot` compiles only the code in the bundle, so the
 * driver's run-time loading is done here ahead of time. The real driver runs
 * in node:vm with a runString that records, instead of evaluating, the scripts
 * it would give a fresh realm: its preamble, the workload files (its
 * `load("<file>")` lines, replaced by the file text) and its own runner loop.
 * Each realm becomes a function called with `this` = globalThis, run one after
 * the other (a grouped benchmark has several); `top.currentResolve` collects
 * the iteration times and a tail scores them as the driver's DefaultBenchmark,
 * WSLBenchmark and GroupedBenchmark do, printing the driver's score lines.
 * Preloaded resources are inlined and served by `read`, as the shell would. */

const BINARY_EXT = /\.(wasm|glb|bin|raw|onnx|png|jpg|ttf|dat)$/;

function captureScripts(name, params) {
  const ctx = driverContext(params);
  const caps = [];
  ctx.runString = () => {
    const realm = { scripts: [] };
    realm.loadString = (s) => realm.scripts.push(String(s));
    caps.push(realm);
    return realm;
  };
  const meta = JSON.parse(
    vm.runInContext(
      `(() => {
        const b = BENCHMARKS.find((x) => x.name === ${JSON.stringify(name)});
        b.prefetchResourcesForShell();
        const subs = b.benchmarks || [b];
        for (const x of subs) x.run().catch(() => {});
        return JSON.stringify({
          name: b.name,
          group: !!b.benchmarks,
          subs: subs.map((x) => ({ name: x.name, kind: x.constructor.name,
            iterations: x.iterations, worstCaseCount: x.worstCaseCount ?? 0,
            preload: Object.fromEntries(x.preloadEntries) })),
        });
      })()`,
      ctx,
    ),
  );
  if (caps.length !== meta.subs.length)
    throw new Error(`captured ${caps.length} realms for ${meta.subs.length} benchmarks`);
  return { meta, realms: caps.map((c) => c.scripts) };
}

function readResource(p) {
  const rel = p.replace(/^\.\//, "");
  const a = join(JS3, rel);
  return readFileSync(existsSync(a) ? a : join(ASSETS, rel));
}

const SCORE_TAIL = `
function __js3Score(meta, results) {
  const toScore = (t) => 5000 / Math.max(t, 1);
  const mean = (a) => a.reduce((x, y) => x + y, 0) / a.length;
  const geomean = (a) => a.reduce((x, y) => x * y, 1) ** (1 / a.length);
  const per = meta.subs.map((sub, i) => {
    const r = results[i];
    if (sub.kind === "WSLBenchmark")
      return { Stdlib: toScore(r[0]), MainRun: toScore(r[1]) };
    const s = { First: toScore(r[0]) };
    const rest = r.slice(1).sort((a, b) => (a < b ? 1 : -1));
    if (sub.worstCaseCount) s.Worst = toScore(mean(rest.slice(0, sub.worstCaseCount)));
    if (sub.iterations > 1) s.Average = toScore(mean(rest));
    return s;
  });
  const sub = {};
  for (const p of per) for (const k in p) (sub[k] ??= []).push(p[k]);
  for (const k in sub) sub[k] = geomean(sub[k]);
  const score = geomean(Object.values(sub));
  const pad = (l) => (meta.name + " " + l).padEnd(45);
  for (const k in sub)
    __js3Print(pad(k + "-Score"), (sub[k].toFixed(2) + " pts").padStart(11));
  __js3Print(pad("Score"), (score.toFixed(2) + " pts").padStart(11));
}
`;

/* CommonJS wrapper names hidden from the workloads (realm function
 * parameters left undefined): UMD bundles then take their global branch,
 * and code that probes for Node (typescript's sys) sees a plain shell. */
const CJS_HIDDEN = ["exports", "module", "define", "require", "process", "__filename", "__dirname"];

let acorn;
/* Names a script declares at top level (var, function, class, let, const;
 * a const name comes back as "const NAME" and gets no setter).
 * In a shell these are globals: other scripts, eval'd code and globalThis.X
 * lookups see them. The entry runs a realm's scripts in one function, so
 * __js3Expose mirrors each name onto globalThis as a live accessor. */
function topLevelNames(text) {
  if (!acorn) {
    const require = createRequire(import.meta.url);
    try {
      acorn = require("acorn");
    } catch {
      throw new Error("bun-canary-aot needs the acorn package (npm install at the repo root)");
    }
  }
  const ast = acorn.parse(text, {
    ecmaVersion: "latest",
    sourceType: "script",
    allowHashBang: true,
    allowReturnOutsideFunction: true,
  });
  const out = [];
  const consts = [];
  const pat = (p) => {
    if (!p) return;
    if (p.type === "Identifier") out.push(p.name);
    else if (p.type === "ObjectPattern") p.properties.forEach((q) => pat(q.type === "RestElement" ? q.argument : q.value));
    else if (p.type === "ArrayPattern") p.elements.forEach(pat);
    else if (p.type === "RestElement") pat(p.argument);
    else if (p.type === "AssignmentPattern") pat(p.left);
  };
  const varsIn = (n) => {
    /* var declarations hoist out of blocks, loops and try statements. */
    if (!n || typeof n.type !== "string") return;
    if (n.type === "VariableDeclaration") {
      if (n.kind === "var") n.declarations.forEach((d) => pat(d.id));
      return;
    }
    if (/Function|Class/.test(n.type)) return;
    for (const k in n) {
      const v = n[k];
      if (Array.isArray(v)) v.forEach(varsIn);
      else if (v && typeof v.type === "string" && k !== "test") varsIn(v);
    }
  };
  for (const st of ast.body) {
    if (st.type === "FunctionDeclaration" || st.type === "ClassDeclaration") out.push(st.id.name);
    else if (st.type === "VariableDeclaration") {
      const from = out.length;
      st.declarations.forEach((d) => pat(d.id));
      if (st.kind === "const") consts.push(...out.slice(from));
    } else varsIn(st);
  }
  return out.map((n) => (consts.includes(n) ? "const " + n : n));
}

/* Writes build/aot/<name>/entry.cjs (and returns its path). The entry is
 * CommonJS so the workloads run in sloppy mode, as scripts do in a shell
 * (implicit globals such as `setupEngine = ...`); see CJS_HIDDEN and
 * topLevelNames for how each realm gets shell-script globals. */
export function aotEntry(name, { iterations = null, worstCase = null } = {}) {
  const params = {};
  if (iterations != null) params.testIterationCount = iterations;
  if (worstCase != null) params.testWorstCaseCount = worstCase;
  const { meta, realms } = captureScripts(name, params);
  const res = {};
  for (const sub of meta.subs)
    for (const p of Object.values(sub.preload)) {
      const buf = readResource(p);
      res[p] = BINARY_EXT.test(p)
        ? { b64: buf.toString("base64") }
        : { text: buf.toString("utf8") };
    }
  const loadRe = /^load\("([^"]+)"\);$/;
  const fns = realms.map((scripts) => {
    const texts = scripts.map((s) => {
      const m = loadRe.exec(s.trim());
      return m ? readResource(m[1]).toString("utf8") : s;
    });
    const all = new Set(texts.flatMap(topLevelNames));
    const names = [...all].filter((n) => !all.has("const " + n));
    return (
      /* The scripts run in an inner function: a workload may declare a
         hidden name itself (let module = ...), which would clash with a
         parameter of the same function. */
      `function (${CJS_HIDDEN.join(", ")}) { return function () {\n` +
      `__js3Expose({${names.map((n) => n.startsWith("const ") ? `get ${n.slice(6)}() { return ${n.slice(6)}; }` : `get ${n}() { return ${n}; }, set ${n}(v) { ${n} = v; }`).join(", ")}});\n` +
      texts.join("\n;\n") +
      "\n}.call(this); }"
    );
  });
  const out = `// JetStream 3 ${meta.name}: generated by tools/benchmark/jetstream3/jetstream3.mjs for bun build --aot.
// Workload sources: JetStream (WebKit), see tools/benchmark/jetstream3/JetStream/LICENSE.
/* Bound now: workloads may replace console (console.log = print). */
const __js3Out = console.log.bind(console), __js3Err = console.error.bind(console);
globalThis.print ??= (...a) => __js3Out(a.join(" "));
globalThis.printErr ??= (...a) => __js3Err(a.join(" "));
const __js3Print = (...a) => __js3Out(a.join(" "));
globalThis.self = globalThis;
function __js3Expose(acc) {
  for (const [n, d] of Object.entries(Object.getOwnPropertyDescriptors(acc))) {
    const g = Object.getOwnPropertyDescriptor(globalThis, n);
    if (g && !g.configurable) continue;
    Object.defineProperty(globalThis, n, { get: d.get, set: d.set, configurable: true, enumerable: true });
  }
}
const __js3Res = ${JSON.stringify(res)};
globalThis.read = function (path, mode) {
  const r = __js3Res[path];
  if (!r) throw new Error("read: no resource " + path);
  if (mode === "binary") {
    const u8 = r.b64 != null ? Uint8Array.from(atob(r.b64), (c) => c.charCodeAt(0)) : new TextEncoder().encode(r.text);
    return u8.buffer;
  }
  return r.b64 != null ? atob(r.b64) : r.text;
};
globalThis.readFile = (p) => read(p);
${SCORE_TAIL}
const __js3Meta = ${JSON.stringify(meta)};
const __js3Realms = [
${fns.join(",\n")}
];
/* The driver gives each sub-benchmark of a group a fresh realm; here they
   share one, so the global object and the built-ins (constructors, their
   prototypes, Math, JSON, Reflect) are restored after each one: a
   sub-benchmark that adds Object.prototype.toJSONString must not leak into
   the next one's for-in. */
function __js3Snapshot() {
  const objs = new Set([globalThis, Math, JSON, Reflect]);
  for (const k of Object.getOwnPropertyNames(globalThis)) {
    const d = Object.getOwnPropertyDescriptor(globalThis, k);
    const v = d && d.value;
    if (typeof v === "function" && /^[A-Z]/.test(k)) {
      objs.add(v);
      if (v.prototype && typeof v.prototype === "object") objs.add(v.prototype);
    }
  }
  return [...objs].map((o) => [o, Object.getOwnPropertyDescriptors(o)]);
}
function __js3Restore(snap) {
  for (const [o, saved] of snap) {
    for (const k of Reflect.ownKeys(o))
      if (!Object.hasOwn(saved, k)) try { delete o[k]; } catch {}
    for (const k of Reflect.ownKeys(saved)) {
      const cur = Object.getOwnPropertyDescriptor(o, k);
      const d = saved[k];
      if (cur && cur.value === d.value && cur.get === d.get && cur.set === d.set) continue;
      try { Object.defineProperty(o, k, d); } catch {}
    }
  }
}
(async () => {
  const results = [];
  const snap = __js3Realms.length > 1 ? __js3Snapshot() : null;
  for (const realm of __js3Realms) {
    results.push(await new Promise((resolve, reject) => {
      globalThis.top = { currentResolve: resolve, currentReject: reject };
      realm.call(globalThis);
    }));
    if (snap) __js3Restore(snap);
  }
  __js3Score(__js3Meta, results);
})().catch((e) => {
  __js3Print("JetStream3 failed: " + e);
  __js3Print(e && e.stack);
  process.exitCode = 1;
});
`;
  const dir = join(BUILD, "aot", name);
  mkdirSync(dir, { recursive: true });
  const p = join(dir, "entry.cjs");
  writeFileSync(p, out);
  return p;
}

export function bunCanaryBin() {
  return (
    process.env.BUN_CANARY_BIN ||
    join(process.env.HOME || "", ".local/bun-canary/bun-linux-x64/bun")
  );
}

/* Compiles a workload's entry with `bun build --compile --aot` (canary).
 * Rebuilds only when the entry changed. Writes explain.json (--explain=json)
 * and build.log next to it. Returns { exe, buildMs, bytes } or { error }. */
export function buildBunCanaryAot(name, opts = {}) {
  let entry;
  try {
    entry = aotEntry(name, opts);
  } catch (e) {
    return { error: `entry: ${e.message}` };
  }
  const dir = dirname(entry);
  const exe = join(dir, "workload");
  const stamp = join(dir, "entry.md5");
  const text = createHash("md5").update(readFileSync(entry)).digest("hex");
  if (existsSync(exe) && existsSync(stamp) && readFileSync(stamp, "utf8") === text)
    return record(name, { exe, buildMs: 0, bytes: statSync(exe).size, cached: true });
  const t0 = Date.now();
  const r = spawnSync(
    bunCanaryBin(),
    ["build", "--compile", "--aot", "--explain=json", entry, "--outfile", exe],
    { encoding: "utf8", maxBuffer: 1 << 30, cwd: dir },
  );
  const buildMs = Date.now() - t0;
  /* --explain=json writes one JSON object per line on stdout, among the
     build's progress lines. */
  const lines = (r.stdout || "").split("\n");
  const isJson = (l) => l.startsWith("{") || l.startsWith("[");
  writeFileSync(join(dir, "explain.json"), lines.filter(isJson).join("\n") + "\n");
  writeFileSync(
    join(dir, "build.log"),
    lines.filter((l) => !isJson(l)).join("\n") + (r.stderr || ""),
  );
  if (r.status !== 0 || !existsSync(exe))
    return record(name, {
      error: `bun build --aot failed: ${((r.stderr || "") + (r.stdout || "") + (r.error || "")).trim().split("\n").filter((l) => !isJson(l)).slice(-3).join(" | ").slice(0, 300)}`,
      buildMs,
    });
  writeFileSync(stamp, text);
  return record(name, { exe, buildMs, bytes: statSync(exe).size });
}

/* build/aot/builds.json: the last build of each workload (time, size, error). */
function record(name, r) {
  const p = join(BUILD, "aot", "builds.json");
  let all = {};
  try {
    all = JSON.parse(readFileSync(p, "utf8"));
  } catch {}
  all[name] = { buildMs: r.buildMs, bytes: r.bytes, cached: !!r.cached, error: r.error };
  mkdirSync(dirname(p), { recursive: true });
  writeFileSync(p, JSON.stringify(all, null, 2));
  return r;
}
