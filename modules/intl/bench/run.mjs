#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The Intl benchmark runner.
 *
 * PURPOSE
 *   Answer "how far is `modules/intl` from node?" per service and per
 *   operation kind, repeatably, by anyone, in one command. The bar is node --
 *   V8 with ICU compiled in -- because that is the honest ceiling for a JS
 *   layer over a platform backend. Hermes ships four services to our ten and is
 *   not the comparison that matters.
 *
 *   Motivated by: the module was functionally complete and entirely unmeasured.
 *   Findings live in `docs/intl-vs-node.md`.
 *
 * WHAT IT MEASURES, IN THREE SEPARATE ARMS
 *   warm   steady-state ns/op, from `harness.js`, N repeated *process*
 *          invocations per engine. Reported as median-of-run-medians with the
 *          run-to-run spread beside it: a delta smaller than the spread is not
 *          a result.
 *   cold   whole-process wall time for a script that touches Intl exactly once,
 *          net of the same script with the Intl line removed. This is the
 *          startup/TTI question and it is deliberately not blended with warm.
 *   mem    peak RSS via /usr/bin/time -l (macOS) or -v (Linux). A 1.1x speedup
 *          that doubles memory is a regression on a phone.
 *
 * FAIRNESS, STATED SO NOBODY HAS TO RECONSTRUCT IT
 *   - Identical source. harness.js + workload + harness-tail.js are
 *     concatenated into ONE temporary file and that exact file is passed to
 *     every engine. No per-engine preamble, no conditional compilation.
 *   - Identical measurement boundary. `Date.now()` inside the script on both
 *     sides, around the same loop, with the same calibration. The process
 *     spawn, engine startup and script parse are outside every warm number and
 *     are the entire content of the cold arm.
 *   - Loop overhead is reported, not subtracted. QuickJS interprets the
 *     harness loop and V8 compiles it; `__loop_overhead` is the size of that
 *     confounder and it is on every table.
 *   - No AOT asymmetry. Neither side gets precompiled bytecode for the
 *     workload. The module's own JS layer IS precompiled in both the qjs arms,
 *     because that is what ships.
 *
 * VALIDATION THE RUNNER PERFORMS ON EVERY RUN
 *   - exit status 0, or the run is discarded and the whole invocation fails;
 *   - a `#END` sentinel line, or the run is discarded -- a workload that threw
 *     halfway would otherwise print a plausible partial table;
 *   - the expected row set, identical between runs and between engines;
 *   - the per-row sink, identical across runs of one engine (a differing sink
 *     means the body is not deterministic and its timing is not a measurement)
 *     and, unless the row is flagged `sinkMayDiffer`, identical across engines
 *     (a free differential test against ICU).
 *
 * USAGE
 *   node modules/intl/bench/run.mjs                       # everything, 5 runs
 *   node modules/intl/bench/run.mjs --runs 9
 *   node modules/intl/bench/run.mjs --workload numberformat
 *   node modules/intl/bench/run.mjs --arm warm --json out.json
 *   node modules/intl/bench/run.mjs --baseline modules/intl/bench/baseline.json
 *   node modules/intl/bench/run.mjs --qjs build-intl/modules/intl/intl-cli
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   It does not run on a device, it does not measure Android, and it does not
 *   profile. It reports where time goes at the granularity of one public API
 *   call; attributing time *within* a call is a profiler's job and the answer
 *   belongs in a document, not here.
 */

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "..", "..", "..");

// ---------------------------------------------------------------- arguments

const argv = process.argv.slice(2);
function opt(name, dflt) {
  const i = argv.indexOf("--" + name);
  return i >= 0 && argv[i + 1] !== undefined ? argv[i + 1] : dflt;
}
function flag(name) {
  return argv.includes("--" + name);
}

const RUNS = Number(opt("runs", 5));
const ARM = opt("arm", "all"); // warm | cold | mem | all
const FILTER = opt("workload", null);
const JSON_OUT = opt("json", null);
const BASELINE = opt("baseline", null);
const QJS = path.resolve(
  ROOT,
  opt("qjs", "build-rel/modules/intl/intl-cli-apple")
);
const NODE = opt("node", process.execPath);
const ONLY_ENGINE = opt("engine", null); // qjs | node

if (flag("help")) {
  console.log(fs.readFileSync(fileURLToPath(import.meta.url), "utf8").slice(0, 3800));
  process.exit(0);
}

if (!fs.existsSync(QJS)) {
  console.error(
    `[intl-bench] engine not found: ${QJS}\n` +
      `Build it with:\n` +
      `  cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DRNQJS_INTL_BUILD_CLI=ON \\\n` +
      `        -DREACT_NATIVE_QUICKJS_DIR=$PWD\n` +
      `  cmake --build build-rel -j8 --target intl-cli-apple`
  );
  process.exit(2);
}

const ENGINES = [
  { id: "qjs", cmd: QJS, args: [] },
  { id: "node", cmd: NODE, args: [] },
].filter((e) => !ONLY_ENGINE || e.id === ONLY_ENGINE);

// ---------------------------------------------------------------- utilities

function median(xs) {
  const s = [...xs].sort((a, b) => a - b);
  return s.length % 2 ? s[s.length >> 1] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2;
}
function pct(xs, p) {
  const s = [...xs].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.floor((p / 100) * s.length))];
}
function fmtNs(ns) {
  if (ns === null || ns === undefined || !isFinite(ns)) return "-";
  if (ns >= 1e6) return (ns / 1e6).toFixed(2) + " ms";
  if (ns >= 1e3) return (ns / 1e3).toFixed(2) + " µs";
  return ns.toFixed(1) + " ns";
}
function pad(s, w, right) {
  s = String(s);
  return right ? s.padStart(w) : s.padEnd(w);
}

const TMP = fs.mkdtempSync(path.join(os.tmpdir(), "intl-bench-"));
process.on("exit", () => {
  try {
    fs.rmSync(TMP, { recursive: true, force: true });
  } catch {}
});

// ------------------------------------------------------------ the warm arm

const HARNESS = fs.readFileSync(path.join(HERE, "harness.js"), "utf8");
const TAIL = fs.readFileSync(path.join(HERE, "harness-tail.js"), "utf8");

function assembleWorkload(file) {
  const body = fs.readFileSync(path.join(HERE, "workloads", file), "utf8");
  const out = path.join(TMP, file);
  fs.writeFileSync(out, HARNESS + "\n" + body + "\n" + TAIL);
  return out;
}

/** One process invocation. Returns {rows: Map<name,{min,med,n,sink,flags}>}. */
function runOnce(engine, scriptPath) {
  const r = spawnSync(engine.cmd, [...engine.args, scriptPath], {
    encoding: "utf8",
    maxBuffer: 64 * 1024 * 1024,
  });
  if (r.error) throw new Error(`${engine.id}: ${r.error.message}`);
  if (r.status !== 0) {
    throw new Error(
      `${engine.id} exited ${r.status} on ${path.basename(scriptPath)}\n` +
        (r.stderr || "").split("\n").slice(0, 12).join("\n")
    );
  }
  const lines = (r.stdout || "").split("\n");
  const rows = new Map();
  let end = null;
  for (const line of lines) {
    if (line.startsWith("#ROW\t")) {
      const [, name, min, med, n, reps, sink, flags] = line.split("\t");
      rows.set(name, {
        min: Number(min),
        med: Number(med),
        n: Number(n),
        reps: Number(reps),
        sink,
        flags,
      });
    } else if (line.startsWith("#END\t")) {
      end = Number(line.slice(5));
    }
  }
  if (end === null) {
    throw new Error(
      `${engine.id}: no #END sentinel from ${path.basename(scriptPath)} ` +
        `(the run produced ${rows.size} rows and then stopped). stderr:\n` +
        (r.stderr || "(empty)").slice(0, 2000)
    );
  }
  if (end !== rows.size) {
    throw new Error(
      `${engine.id}: #END says ${end} rows, ${rows.size} parsed`
    );
  }
  return rows;
}

function warmArm(workloadFile) {
  const script = assembleWorkload(workloadFile);
  const perEngine = {};
  const problems = [];

  for (const engine of ENGINES) {
    const runs = [];
    for (let i = 0; i < RUNS; i++) runs.push(runOnce(engine, script));

    // Row-set stability across runs of one engine.
    const names = [...runs[0].keys()];
    for (const r of runs) {
      if (r.size !== names.length)
        problems.push(`${engine.id}: row count varied between runs`);
    }

    const agg = new Map();
    for (const name of names) {
      const meds = runs.map((r) => r.get(name).med);
      const mins = runs.map((r) => r.get(name).min);
      const sinks = new Set(runs.map((r) => r.get(name).sink));
      if (sinks.size !== 1) {
        problems.push(
          `${engine.id}/${name}: sink differed between runs ` +
            `(${[...sinks].join(" vs ")}) -- the body is not deterministic, ` +
            `so its timing is not a measurement`
        );
      }
      agg.set(name, {
        med: median(meds),
        min: Math.min(...mins),
        p95: pct(meds, 95),
        max: Math.max(...meds),
        spread: (Math.max(...meds) - Math.min(...meds)) / median(meds),
        n: runs[0].get(name).n,
        sink: runs[0].get(name).sink,
        flags: runs[0].get(name).flags,
        samples: meds,
      });
    }
    perEngine[engine.id] = agg;
  }

  // Cross-engine sink agreement -- a free differential test against ICU.
  if (perEngine.qjs && perEngine.node) {
    for (const [name, a] of perEngine.qjs) {
      const b = perEngine.node.get(name);
      if (!b) {
        problems.push(`row ${name} present in qjs, absent in node`);
        continue;
      }
      if (a.flags !== "sinkMayDiffer" && a.sink !== b.sink) {
        problems.push(
          `SINK DIVERGENCE ${name}: qjs=${a.sink} node=${b.sink} ` +
            `(row is not flagged sinkMayDiffer, so this is a correctness finding)`
        );
      }
    }
  }
  return { perEngine, problems };
}

// ------------------------------------------------------------- the cold arm

/*
 * Cold arm. Each case is a pair: `probe` does the thing once, `control` is the
 * same script with the Intl work removed. The difference is the cost, net of
 * process spawn, engine boot and script parse -- all of which are large and
 * completely different between the two engines, which is exactly why they must
 * be differenced away rather than reported.
 */
const COLD_CASES = [
  {
    id: "intl-untouched",
    control: 'var x=1;if(x!==1)throw 0;print("ok")',
    probe: 'var x=1;if(x!==1)throw 0;print("ok")',
    note: "sanity: control vs control, must be ~0",
  },
  {
    id: "first-read-of-Intl",
    control: 'var t=typeof globalThis;if(t!=="object")throw 0;print("ok")',
    probe: 'var t=typeof Intl;if(t!=="object")throw 0;print("ok")',
    note: "materializes the JS layer (qjs) / nothing (node)",
  },
  {
    id: "first-NumberFormat",
    control: 'var s="1.234,50";if(!s)throw 0;print("ok")',
    probe:
      'var s=new Intl.NumberFormat("de-DE").format(1234.5);if(!s)throw 0;print("ok")',
    note: "first read + first ctor + first format",
  },
  {
    id: "first-DateTimeFormat",
    control: 'var s="1.1.1970";if(!s)throw 0;print("ok")',
    probe:
      'var s=new Intl.DateTimeFormat("de-DE").format(0);if(!s)throw 0;print("ok")',
    note: "first read + first ctor + first format",
  },
  {
    id: "first-toLocaleString",
    control: 'var s=(1234.5).toString();if(!s)throw 0;print("ok")',
    probe: 'var s=(1234.5).toLocaleString("de-DE");if(!s)throw 0;print("ok")',
    note: "the trampoline path: materialization triggered without naming Intl",
  },
];

function timeProcess(engine, source, reps) {
  const f = path.join(TMP, "cold.js");
  fs.writeFileSync(
    f,
    (engine.id === "node"
      ? 'globalThis.print=function(){console.log.apply(console,arguments)};\n'
      : "") + source
  );
  const ts = [];
  for (let i = 0; i < reps; i++) {
    const t0 = process.hrtime.bigint();
    const r = spawnSync(engine.cmd, [...engine.args, f], { encoding: "utf8" });
    const t1 = process.hrtime.bigint();
    if (r.status !== 0)
      throw new Error(`${engine.id} cold arm exited ${r.status}: ${r.stderr}`);
    if (!/\bok\b/.test(r.stdout || ""))
      throw new Error(
        `${engine.id} cold arm printed no "ok" -- the probe did not run. ` +
          `stdout=${JSON.stringify((r.stdout || "").slice(0, 200))}`
      );
    ts.push(Number(t1 - t0) / 1e6);
  }
  return ts;
}

function coldArm(reps) {
  const out = {};
  for (const engine of ENGINES) {
    out[engine.id] = {};
    for (const c of COLD_CASES) {
      const ctl = timeProcess(engine, c.control, reps);
      const prb = timeProcess(engine, c.probe, reps);
      out[engine.id][c.id] = {
        controlMs: median(ctl),
        probeMs: median(prb),
        deltaMs: median(prb) - median(ctl),
        controlMinMs: Math.min(...ctl),
        probeMinMs: Math.min(...prb),
        deltaMinMs: Math.min(...prb) - Math.min(...ctl),
        note: c.note,
      };
    }
  }
  return out;
}

// -------------------------------------------------------------- the mem arm

/*
 * Peak RSS. `leaks` answers "is anything unreachable"; this answers "how much
 * is held", and they are different questions -- @autoreleasepool on the ObjC
 * entry points took 20,000 formatter constructions here from 23.4 MB to
 * 16.6 MB with `leaks` reporting zero either way.
 */
const MEM_CASES = [
  {
    id: "baseline",
    src: 'var s=0;for(var i=0;i<20000;i++)s+=i;if(s<0)throw 0;print("ok")',
  },
  {
    id: "20k-NumberFormat-ctor+format",
    src:
      'var n=0;for(var i=0;i<20000;i++){n+=new Intl.NumberFormat("de-DE",' +
      '{style:"currency",currency:"EUR"}).format(i).length;}if(!n)throw 0;print("ok")',
  },
  {
    id: "20k-DateTimeFormat-ctor+format",
    src:
      'var n=0;for(var i=0;i<20000;i++){n+=new Intl.DateTimeFormat("de-DE")' +
      '.format(i*1000).length;}if(!n)throw 0;print("ok")',
  },
  {
    id: "1-formatter-200k-format",
    src:
      'var f=new Intl.NumberFormat("de-DE");var n=0;' +
      'for(var i=0;i<200000;i++)n+=f.format(i).length;if(!n)throw 0;print("ok")',
  },
  {
    id: "200k-toLocaleString",
    src:
      'var n=0;for(var i=0;i<200000;i++)n+=(i+0.5).toLocaleString("de-DE").length;' +
      'if(!n)throw 0;print("ok")',
  },
];

function peakRssKb(engine, source) {
  const f = path.join(TMP, "mem.js");
  fs.writeFileSync(
    f,
    (engine.id === "node"
      ? 'globalThis.print=function(){console.log.apply(console,arguments)};\n'
      : "") + source
  );
  const isMac = process.platform === "darwin";
  const r = spawnSync(
    "/usr/bin/time",
    isMac ? ["-l", engine.cmd, f] : ["-v", engine.cmd, f],
    { encoding: "utf8" }
  );
  if (r.status !== 0)
    throw new Error(`mem arm ${engine.id} exited ${r.status}: ${r.stderr}`);
  if (!/\bok\b/.test(r.stdout || ""))
    throw new Error(`mem arm ${engine.id} printed no "ok"`);
  const m = isMac
    ? /(\d+)\s+maximum resident set size/.exec(r.stderr)
    : /Maximum resident set size \(kbytes\):\s*(\d+)/.exec(r.stderr);
  if (!m) throw new Error(`could not parse peak RSS from /usr/bin/time output`);
  /*
   * UNITS. macOS `/usr/bin/time -l` reports "maximum resident set size" in
   * BYTES; GNU `time -v` reports it in KILOBYTES. Getting this wrong is easy
   * and silent -- the first version of this function divided the macOS figure
   * by 1024 and labelled the result MB, which printed a 6.5 MB process as
   * "6704 MB" and a 45 MB node as "44848 MB". Both numbers looked wrong enough
   * to catch; a 1024x error in the other direction would not have.
   */
  return isMac ? Number(m[1]) / (1024 * 1024) : Number(m[1]) / 1024;
}

function memArm(reps) {
  const out = {};
  for (const engine of ENGINES) {
    out[engine.id] = {};
    for (const c of MEM_CASES) {
      const xs = [];
      for (let i = 0; i < reps; i++) xs.push(peakRssKb(engine, c.src));
      out[engine.id][c.id] = { peakKb: median(xs), samples: xs };
    }
  }
  return out;
}

// ----------------------------------------------------------------- reporting

function reportWarm(workloadFile, res) {
  const names = [...(res.perEngine.qjs || res.perEngine.node).keys()];
  console.log(`\n### ${workloadFile}  (${RUNS} process runs per engine)`);
  const hdr =
    pad("row", 40) +
    pad("qjs med", 12, true) +
    pad("spread", 9, true) +
    pad("node med", 12, true) +
    pad("spread", 9, true) +
    pad("qjs/node", 10, true) +
    "  n";
  console.log(hdr);
  console.log("-".repeat(hdr.length));
  for (const name of names) {
    const a = res.perEngine.qjs?.get(name);
    const b = res.perEngine.node?.get(name);
    const ratio = a && b ? a.med / b.med : null;
    console.log(
      pad(name, 40) +
        pad(a ? fmtNs(a.med) : "-", 12, true) +
        pad(a ? (a.spread * 100).toFixed(0) + "%" : "-", 9, true) +
        pad(b ? fmtNs(b.med) : "-", 12, true) +
        pad(b ? (b.spread * 100).toFixed(0) + "%" : "-", 9, true) +
        pad(ratio ? ratio.toFixed(2) + "x" : "-", 10, true) +
        "  " +
        (a || b).n
    );
  }
  for (const p of res.problems) console.log("  !! " + p);
}

function reportCold(cold) {
  console.log(`\n### cold arm -- whole-process wall time, net of control`);
  /*
   * Both the median-of-differences and the difference-of-minima are printed.
   * They answer different questions and on a loaded machine they disagree: the
   * median carries the scheduler's tail, the min is the closest this harness
   * gets to the cost with nothing else running. The `intl-untouched` row is
   * control-versus-control and IS the noise floor -- read every other row
   * against it, and disbelieve any delta smaller than it.
   */
  const hdr =
    pad("case", 26) +
    pad("qjs ctl", 11, true) +
    pad("qjs probe", 11, true) +
    pad("qjs Δmed", 11, true) +
    pad("qjs Δmin", 11, true) +
    pad("node Δmed", 11, true) +
    pad("node Δmin", 11, true) +
    "  note";
  console.log(hdr);
  console.log("-".repeat(hdr.length));
  for (const c of COLD_CASES) {
    const a = cold.qjs?.[c.id];
    const b = cold.node?.[c.id];
    console.log(
      pad(c.id, 26) +
        pad(a ? a.controlMs.toFixed(2) + " ms" : "-", 11, true) +
        pad(a ? a.probeMs.toFixed(2) + " ms" : "-", 11, true) +
        pad(a ? a.deltaMs.toFixed(2) + " ms" : "-", 11, true) +
        pad(a ? a.deltaMinMs.toFixed(2) + " ms" : "-", 11, true) +
        pad(b ? b.deltaMs.toFixed(2) + " ms" : "-", 11, true) +
        pad(b ? b.deltaMinMs.toFixed(2) + " ms" : "-", 11, true) +
        "  " +
        c.note
    );
  }
}

function reportMem(mem) {
  console.log(`\n### mem arm -- peak RSS`);
  const hdr =
    pad("case", 34) + pad("qjs", 12, true) + pad("node", 12, true) + pad("qjs/node", 10, true);
  console.log(hdr);
  console.log("-".repeat(hdr.length));
  for (const c of MEM_CASES) {
    const a = mem.qjs?.[c.id];
    const b = mem.node?.[c.id];
    console.log(
      pad(c.id, 34) +
        pad(a ? a.peakKb.toFixed(1) + " MB" : "-", 12, true) +
        pad(b ? b.peakKb.toFixed(1) + " MB" : "-", 12, true) +
        pad(a && b ? (a.peakKb / b.peakKb).toFixed(2) + "x" : "-", 10, true)
    );
  }
}

// ---------------------------------------------------------------------- main

const workloads = fs
  .readdirSync(path.join(HERE, "workloads"))
  .filter((f) => f.endsWith(".js"))
  .filter((f) => !FILTER || f.includes(FILTER))
  .sort();

if (workloads.length === 0) {
  console.error(`[intl-bench] no workloads matched ${FILTER}`);
  process.exit(2);
}

const result = {
  meta: {
    date: new Date().toISOString(),
    platform: `${os.platform()} ${os.release()} ${os.arch()}`,
    cpus: os.cpus()[0]?.model,
    loadavg: os.loadavg(),
    node: process.version,
    nodeIcu: process.versions.icu,
    qjs: QJS,
    qjsBackend: (() => {
      const r = spawnSync(QJS, ["--print-backend", "-e", "0"], {
        encoding: "utf8",
      });
      return (r.stdout || "").trim();
    })(),
    runs: RUNS,
    arm: ARM,
  },
  warm: {},
  cold: null,
  mem: null,
};

console.log(`[intl-bench] ${result.meta.qjsBackend || "backend=?"}  node ${process.version} (ICU ${process.versions.icu})`);
console.log(`[intl-bench] loadavg ${result.meta.loadavg.map((x) => x.toFixed(2)).join(" ")}`);

let anyProblem = false;

if (ARM === "warm" || ARM === "all") {
  for (const w of workloads) {
    const res = warmArm(w);
    reportWarm(w, res);
    if (res.problems.length) anyProblem = true;
    result.warm[w] = {};
    for (const [eid, agg] of Object.entries(res.perEngine)) {
      result.warm[w][eid] = Object.fromEntries(agg);
    }
    result.warm[w].problems = res.problems;
  }
}

if (ARM === "cold" || ARM === "all") {
  result.cold = coldArm(Math.max(5, RUNS));
  reportCold(result.cold);
}

if (ARM === "mem" || ARM === "all") {
  result.mem = memArm(Math.max(3, Math.min(RUNS, 5)));
  reportMem(result.mem);
}

if (JSON_OUT) {
  fs.writeFileSync(path.resolve(ROOT, JSON_OUT), JSON.stringify(result, null, 2));
  console.log(`\n[intl-bench] wrote ${JSON_OUT}`);
}

if (BASELINE) {
  const base = JSON.parse(fs.readFileSync(path.resolve(ROOT, BASELINE), "utf8"));
  console.log(`\n### vs baseline ${BASELINE} (${base.meta?.date})`);
  const hdr = pad("row", 44) + pad("base", 12, true) + pad("now", 12, true) + pad("change", 11, true);
  console.log(hdr);
  console.log("-".repeat(hdr.length));
  for (const [w, engines] of Object.entries(result.warm)) {
    const bw = base.warm?.[w];
    if (!bw) continue;
    for (const [name, cur] of Object.entries(engines.qjs || {})) {
      const old = bw.qjs?.[name];
      if (!old) continue;
      const ch = cur.med / old.med;
      // The threshold is the larger of 5% and the two runs' own spreads: a
      // change smaller than the inter-run spread is not a result.
      const noise = Math.max(0.05, old.spread || 0, cur.spread || 0);
      const tag = ch < 1 - noise ? "  FASTER" : ch > 1 + noise ? "  SLOWER" : "";
      console.log(
        pad(name, 44) +
          pad(fmtNs(old.med), 12, true) +
          pad(fmtNs(cur.med), 12, true) +
          pad(ch.toFixed(2) + "x", 11, true) +
          tag
      );
    }
  }
}

if (anyProblem) {
  console.error(
    `\n[intl-bench] FAILED: one or more validation problems above ` +
      `(sink divergence or unstable rows). The timings on this run are not trustworthy.`
  );
  process.exit(1);
}
