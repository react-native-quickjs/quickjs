#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import { execFileSync, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync, statSync } from 'node:fs';
import { join, dirname, resolve, relative, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import * as device from './lib/device.mjs';
import * as hermes from './lib/hermes.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..', '..');
const WORKLOADS = join(HERE, 'workloads');
const OCTANE = join(HERE, 'octane');
const KERNELS = join(HERE, 'kernels');
const LIB = join(HERE, 'lib');
const HARNESS = join(LIB, 'harness.js');

function fail(msg, code = 2) { console.error(`bench: ${msg}`); process.exit(code); }

function parseArgs(argv) {
  const out = { _: [], rows: [], skip: [], engines: [], suites: [], ab: [],
                list: false, json: false, save: false, against: null,
                mem: false, no_mem: false, reps: 1, min_ms: 100, min_ms_set: false,
                scale: 1, scale_set: false, n: null, ab_label: null,
                device: false, ab_only: false, ab_baseline: null };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--rows') out.rows.push(argv[++i]);
    else if (a === '--skip') out.skip.push(argv[++i]);
    else if (a === '--engine') out.engines.push(argv[++i]);
    else if (a === '--suite') out.suites.push(argv[++i]);
    else if (a === '--ab') out.ab.push(argv[++i]);
    else if (a === '--list') out.list = true;
    else if (a === '--json') out.json = true;
    else if (a === '--save') { const nx = argv[i + 1]; out.save = (nx && !nx.startsWith('--')) ? (i++, nx) : true; }
    else if (a === '--against') out.against = argv[++i];
    else if (a === '--mem') out.mem = true;
    else if (a === '--no-mem') out.no_mem = true;
    else if (a === '--device') out.device = true;
    else if (a === '--reps') out.reps = Math.max(1, Number(argv[++i]));
    else if (a === '--min-ms') { out.min_ms = Number(argv[++i]); out.min_ms_set = true; }
    else if (a === '--scale') { out.scale = Number(argv[++i]); out.scale_set = true; }
    else if (a === '--n') out.n = argv[++i];
    else if (a === '--label') out.ab_label = argv[++i];
    else if (a === '--baseline') out.ab_baseline = argv[++i];
    else if (a === '--ab-only') out.ab_only = true;
    else if (a === '-h' || a === '--help') out.help = true;
    else if (a.startsWith('--')) fail(`unknown flag: ${a}`);
    else out._.push(a);
  }
  return out;
}

function usage() {
  console.log(`usage: bench.mjs [suites...] [options]

suites:
  react minireact rn primitives data strings surface suspense modern arrayholes
  octane compiler jit calls startup

options:
  --rows a,b,c         only run rows matching (substring)
  --skip a,b,c         exclude rows matching
  --engine qjs|hermes  add an engine (qjs always added)
  --ab a=/x,b=/y       A/B compare two binaries (alias for --engine)
  --reps N             process reps (default 1)
  --min-ms N           workload calibration floor (default 100)
  --scale N            multiply every workload's n (default 1)
  --n N                one-shot for startup: bundle path
  --mem / --no-mem     capture/don't capture engine memory per row
  --save [file]        record to baseline
  --against [file]     compare, exit 1 on regression (pinned iters)
  --label <name>       label the --ab arm
  --baseline <file>    baseline to read pinned iters from
  --json               machine-readable
  --list               list rows and exit
  --device             run on a connected Android device
  -h, --help           this help`);
}

const SUITES = [
  { kind: 'workload', name: 'react',       file: '10-react.js' },
  { kind: 'workload', name: 'minireact',   file: '11-minireact.js' },
  { kind: 'workload', name: 'rn',          file: '20-rn-props.js' },
  { kind: 'workload', name: 'arrayholes',  file: '25-array-holes.js' },
  { kind: 'workload', name: 'primitives',  file: '30-primitives.js' },
  { kind: 'workload', name: 'data',        file: '40-data.js' },
  { kind: 'workload', name: 'strings',     file: '50-strings.js' },
  { kind: 'workload', name: 'surface',     file: '60-surface.js' },
  { kind: 'workload', name: 'suspense',    file: '70-suspense.js' },
  { kind: 'workload', name: 'modern',      file: '80-modern.js' },
  { kind: 'octane',    name: 'octane' },
  { kind: 'kernels',   name: 'compiler', file: 'compiler_bench.js' },
  { kind: 'kernels',   name: 'jit',      file: 'jit_bench.js' },
  { kind: 'calls',     name: 'calls' },
  { kind: 'startup',   name: 'startup' },
];

function selectedSuites(args) {
  const reqs = (args.suites.length ? args.suites : []).concat(args._);
  if (reqs.length === 0) return SUITES.slice();
  const out = [];
  for (const r of reqs) {
    const s = SUITES.find(x => x.name === r);
    if (!s) fail(`unknown suite: ${r}`);
    out.push(s);
  }
  return out;
}

function allRows(suites) {
  const out = [];
  for (const s of suites) {
    if (s.kind === 'workload') {
      const text = readFileSync(join(WORKLOADS, s.file), 'utf8');
      for (const m of text.matchAll(/bench\(\{[\s\S]{0,120}?name:\s*['"]([^'"]+)['"]/g))
        out.push({ id: m[1], suite: s.name, name: m[1] });
    } else if (s.kind === 'octane') {
      if (!existsSync(join(OCTANE, 'corpus', 'MANIFEST.json'))) return out;
      for (const f of readFileSync(join(OCTANE, 'corpus', 'MANIFEST.json'), 'utf8').matchAll(/"row":\s*"([^"]+)"/g))
        out.push({ id: `octane/${f[1]}`, suite: 'octane', name: f[1] });
    } else if (s.kind === 'kernels') {
      const text = readFileSync(join(KERNELS, s.file), 'utf8');
      for (const m of text.matchAll(/bench\("([^"]+)"/g))
        out.push({ id: `${s.name}/${m[1]}`, suite: s.name, name: m[1] });
    } else if (s.kind === 'calls') {
      for (const sh of knownCalls()) out.push({ id: `calls/${sh}`, suite: 'calls', name: sh });
    } else if (s.kind === 'startup') {
      for (const p of ['load-only','register-only','entry','force-all']) out.push({ id: `startup/${p}`, suite: 'startup', name: p });
    }
  }
  return out;
}

let _knownCalls = null;
function knownCalls() {
  if (_knownCalls) return _knownCalls;
  try { _knownCalls = execFileSync(binPath('native-call-bench'), ['--list'], { encoding: 'utf8' }).trim().split('\n'); }
  catch { _knownCalls = []; }
  return _knownCalls;
}

function filterRows(rows, args) {
  const wants = args.rows.flatMap(s => s.split(',')).filter(Boolean);
  const skips = args.skip.flatMap(s => s.split(',')).filter(Boolean);
  return rows.filter(r => {
    if (wants.length && !wants.some(w => r.id.includes(w))) return false;
    if (skips.length && skips.some(w => r.id.includes(w))) return false;
    return true;
  });
}

function binPath(name) {
  const candidates = [join(ROOT, 'build-release', name), join(ROOT, 'build', name)];
  for (const p of candidates) if (existsSync(p)) return p;
  process.stderr.write(`[bench] ${name} not found; building Release binaries...\n`);
  execFileSync('cmake', ['-B', join(ROOT, 'build-release'), '-DCMAKE_BUILD_TYPE=Release'], { stdio: 'inherit' });
  execFileSync('cmake', ['--build', join(ROOT, 'build-release'), '-j8', '--target', name], { stdio: 'inherit' });
  const p = join(ROOT, 'build-release', name);
  if (!existsSync(p)) fail(`failed to build ${name}`);
  return p;
}

const MB = 64 * 1024 * 1024;
function launch(engine, argv, opts = {}) {
  if (engine.device && engine.kind === 'hermes') return device.deviceHermesRun(engine.device.hermes, argv);
  if (engine.device) return device.deviceRun(engine.device.qjs, argv);
  return spawnSync(engine.bin, argv, { encoding: 'utf8', maxBuffer: MB, ...opts });
}

function enginesFromArgs(args, hermesHost) {
  const out = [];
  const hasAb = args.ab.length > 0;
  if (!hasAb) out.push({ id: 'qjs', bin: binPath('qjs-bench'), kind: 'qjs', extra: [] });
  for (const a of args.ab) {
    const m = a.match(/^([^=]+)=(.+)$/);
    if (!m) fail(`--ab expects label=path, got ${a}`);
    out.push({ id: m[1], bin: resolve(m[2]), kind: 'qjs', extra: [] });
  }
  for (const e of args.engines) {
    if (e === 'hermes') {
      const h = process.env.HERMES_BIN || hermesHost?.hermes || '/Users/ammarahmed/Work/hermes/hermes/build_release/bin/hermes';
      if (h && existsSync(h)) out.push({ id: 'hermes', bin: h, kind: 'hermes', extra: ['-O'] });
      else console.error(`bench: hermes not found at ${h}, skipping`);
    } else if (e === 'qjs') { /* already handled */ }
    else fail(`unknown engine: ${e}`);
  }
  if (out.length === 0) out.push({ id: 'qjs', bin: binPath('qjs-bench'), kind: 'qjs', extra: [] });
  return out;
}

function loadBaseline(path) {
  if (!path) return null;
  const p = path === true ? join(HERE, 'baseline.json') : path;
  if (!existsSync(p)) return null;
  return JSON.parse(readFileSync(p, 'utf8'));
}

function pinnedFromBaseline(baseline, suite, row) {
  if (!baseline || !baseline.rows) return null;
  for (const r of baseline.rows) {
    if (r.name === row && (r.suite === suite || !r.suite) && r.pinned_iters) return r.pinned_iters;
  }
  return null;
}

function median(xs) {
  const a = xs.slice().sort((x, y) => x - y);
  const n = a.length;
  if (n === 0) return 0;
  return n % 2 ? a[(n - 1) >> 1] : (a[n / 2 - 1] + a[n / 2]) / 2;
}

function geomean(xs) {
  const ls = xs.filter(x => x > 0 && isFinite(x));
  if (ls.length === 0) return 0;
  return Math.exp(ls.reduce((s, x) => s + Math.log(x), 0) / ls.length);
}

function runWorkload(suite, row, engine, args, baselineIters) {
  const src = readFileSync(join(WORKLOADS, suite.file), 'utf8');
  const harness = readFileSync(HARNESS, 'utf8');
  const tail = '__BENCH_REPORT__();';
  const pinnedMap = baselineIters || {};
  const pinned = JSON.stringify(pinnedMap);
  const combined = harness.replace('var __SINK__', `var __BENCH_PINNED_ITERS__ = ${pinned};\nvar __SINK__`) + src + tail;
  const tmp = join(tmpdir(), `rnqjs-bench-${process.pid}-${Math.random().toString(36).slice(2)}.js`);
  writeFileSync(tmp, combined);
  const wantMem = args.mem || engine.kind === 'qjs';
  const argv = engine.extra.concat(wantMem ? ['--mem'] : [], [tmp]);
  const p = launch(engine, argv);
  try { spawnSync('rm', [tmp], { stdio: 'ignore' }); } catch {}
  if (p.status !== 0 && p.stdout === '') {
    return { error: 'engine failed: ' + (p.stderr || '').split('\n').slice(0, 3).join(' | ') };
  }
  const results = {};
  let mem = null;
  const scan = (s) => { for (const line of (s || '').split('\n')) {
    if (line.startsWith('##BENCH## ')) {
      try { const b = JSON.parse(line.slice('##BENCH## '.length)); if (b.name) results[b.name] = { ns: b.medNs, iters: b.iters, spread: b.spreadPct, error: b.error }; } catch {}
    }
    else if (line.startsWith('##MEM## ')) { try { mem = JSON.parse(line.slice('##MEM## '.length)); } catch {} }
  }};
  scan(p.stdout);
  if (!mem) scan(p.stderr);
  if (Object.keys(results).length === 0) return { error: 'no ##BENCH## lines', stderr: (p.stderr || '').slice(0, 200) };
  return { results, mem };
}

function runOctane(suite, row, engine, args) {
  const corpusDir = join(OCTANE, 'corpus');
  const file = join(corpusDir, `${row}.js`);
  if (!existsSync(file)) return { error: 'row not in corpus' };
  const wantMem = args.no_mem ? false : (args.mem || engine.kind === 'qjs');
  const argv = engine.extra.concat(['--stats'], wantMem ? ['--mem'] : [], [file]);
  const p = launch(engine, argv, { timeout: 300000 });
  if (p.status !== 0) return { error: 'engine failed', stderr: (p.stderr || '').slice(0, 200) };
  let stats = null;
  let score = null;
  let mem = null;
  for (const line of [...(p.stdout || '').split('\n'), ...(p.stderr || '').split('\n')]) {
    if (line.startsWith('##STATS## ')) {
      try { stats = JSON.parse(line.slice('##STATS## '.length)); } catch {}
    } else if (line.startsWith('##MEM## ')) { try { mem = JSON.parse(line.slice('##MEM## '.length)); } catch {} }
    else if (line.startsWith('Score: ')) {
      score = Number(line.slice(7).trim());
    }
  }
  if (!score) return { error: 'no Score: line', stderr: (p.stderr || '').slice(0, 200) };
  return { score, stats, mem };
}

function runKernels(suite, row, engine, args) {
  const kernelFile = join(KERNELS, suite.file);
  let text = readFileSync(kernelFile, 'utf8');
  if (!new RegExp(`bench\\("${row.replace(/[-/\\^$*+?.()|[\]{}]/g, '\\$&')}",\\s*`).test(text))
    return { error: 'kernel not found' };
  text = text.replace(/var CB_FILTER = "[^"]*";/, `var CB_FILTER = "${row}";`);
  text = text.replace(/var CB_REPS = \d+;/, `var CB_REPS = ${Math.max(1, args.reps)};`);
  const wantMem = args.mem || engine.kind === 'qjs';
  const tmp = join(tmpdir(), `rnqjs-kernel-${process.pid}-${Math.random().toString(36).slice(2)}.js`);
  writeFileSync(tmp, text);
  const argv = engine.extra.concat(wantMem ? ['--mem'] : [], [tmp]);
  const p = launch(engine, argv, { timeout: 300000 });
  try { spawnSync('rm', [tmp], { stdio: 'ignore' }); } catch {}
  if (p.status !== 0 && !(p.stdout || '').includes('##CB2##')) return { error: 'engine failed: ' + (p.stderr || '').split('\n').slice(0, 3).join(' | ') };
  let cb2 = null, mem = null;
  for (const line of [...(p.stdout || '').split('\n'), ...(p.stderr || '').split('\n')]) {
    if (line.startsWith('##CB2## ')) {
      const m = line.split(/\s+/);
      cb2 = { name: row, ms: Number(m[2]), iters: Number(m[3]), sum: m[4], score: Number(m[5]) };
    } else if (line.startsWith('##MEM## ')) { try { mem = JSON.parse(line.slice('##MEM## '.length)); } catch {} }
    else if (line.startsWith('##CBFAIL## ')) return { error: line.slice('##CBFAIL## '.length) };
  }
  if (!cb2 || cb2.ms < 0) return { error: 'no valid ##CB2## line' };
  return { ns: cb2.ms * 1e6, ms: cb2.ms, score: cb2.score, iters: cb2.iters, mem };
}

function runCalls(suite, row, engine, args) {
  const bench = engine.device ? engine.device.calls : binPath('native-call-bench');
  const N1 = 100000, N2 = 1000000;
  function timeAt(n) {
    const p = engine.device
      ? device.deviceCallRun(bench, [row, String(n)])
      : spawnSync(bench, [row, String(n)], { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
    const m = /^##CALL## (\S+) ([\d.]+) (\d+)$/m.exec(p.stdout || '');
    return m ? Number(m[2]) : null;
  }
  const t1 = timeAt(N1), t2 = timeAt(N2);
  if (t1 == null || t2 == null) return { error: 'engine failed' };
  const ns = (t2 * N2 - t1 * N1) / (N2 - N1);
  return { ns: ns > 0 ? ns : t2 };
}

function runStartup(suite, row, engine, args, baselineIters) {
  if (!args.n || !existsSync(args.n)) return { error: 'startup needs a bundle/file (--n <path>)' };
  let best = Infinity, stats = null;
  for (let i = 0; i < (args.reps > 1 ? args.reps : 5); i++) {
    const argv = engine.extra.concat(['--stats'], [args.n]);
    const p = launch(engine, argv, { timeout: 120000 });
    if (p.status !== 0) continue;
    for (const line of (p.stderr || '').split('\n')) {
      if (line.startsWith('##STATS## ')) { try { stats = JSON.parse(line.slice('##STATS## '.length)); } catch {} break; }
    }
    if (stats && stats.evalMs < best) best = stats.evalMs;
  }
  if (!stats) return { error: 'no ##STATS## line' };
  return { ms: best, boot: stats.startupMs, malloc: stats.mallocSize };
}

function runnerFor(suite) {
  if (suite.kind === 'workload') return runWorkload;
  if (suite.kind === 'octane') return runOctane;
  if (suite.kind === 'kernels') return runKernels;
  if (suite.kind === 'calls') return runCalls;
  if (suite.kind === 'startup') return runStartup;
  return () => ({ error: 'no runner' });
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help) { usage(); return; }
  if (args.list) {
    const suites = selectedSuites(args);
    const rows = allRows(suites);
    const filtered = filterRows(rows, args);
    for (const s of suites) {
      console.log(`\n[${s.name}]`);
      for (const r of filtered.filter(x => x.suite === s.name)) console.log('  ' + r.id);
    }
    console.log(`\n${filtered.length} row(s) across ${suites.length} suite(s)`);
    return;
  }

  let hermesHost = null;
  if (args.engines.includes('hermes') && !process.env.HERMES_BIN) {
    try { hermesHost = await hermes.ensureHostHermes(); }
    catch (err) { console.error(`bench: hermes unavailable: ${err.message}`); }
  }
  const engines = enginesFromArgs(args, hermesHost);
  if (args.device) {
    try {
      const built = device.crossBuild(join(ROOT, 'engine', 'quickjs-rel'));
      for (const e of engines) if (e.kind === 'qjs') e.device = built;
      if (args.engines.includes('hermes')) {
        const androidHermes = await hermes.ensureAndroidHermes();
        const he = engines.find(e => e.kind === 'hermes');
        if (he) he.device = { hermes: androidHermes };
      }
    } catch (err) { fail(`device: ${err.message}`); }
  }
  const baseline = loadBaseline(args.against || args.ab_baseline);
  const suites = selectedSuites(args);
  if (suites.some(s => s.kind === 'octane') && !existsSync(join(OCTANE, 'corpus', 'MANIFEST.json'))) {
    process.stderr.write('[bench] building octane corpus...\n');
    execFileSync(process.execPath, [join(OCTANE, 'build-octane.mjs')], { stdio: 'inherit' });
  }
  const all = allRows(suites);
  const rows = filterRows(all, args);
  if (rows.length === 0) fail('no rows after filter');

  const perEngine = {};
  for (const e of engines) perEngine[e.id] = {};

  function pinnedMapForFile(baseline, suiteName) {
    if (!baseline || !baseline.rows) return null;
    const m = {};
    let any = false;
    for (const r of baseline.rows) {
      if (r.suite === suiteName && r.pinned_iters) { m[r.name] = r.pinned_iters; any = true; }
    }
    return any ? m : null;
  }

  let counter = 0, total = rows.length * engines.length;
  for (const s of suites) {
    const suite = s;
    const run = runnerFor(suite);
    const suiteRows = rows.filter(r => r.suite === s.name);
    if (!suiteRows.length) continue;
    if (suite.kind === 'workload') {
      const wanted = new Set(suiteRows.map(r => r.name));
      for (const e of engines) {
        const pinned = baseline ? pinnedMapForFile(baseline, s.name) : null;
        process.stderr.write(`\r[${++counter}/${total}] ${s.name} on ${e.id}   `);
        const r0 = run(suite, null, e, args, pinned);
        if (r0.error) { for (const r of suiteRows) perEngine[e.id][r.id] = { error: r0.error }; }
        else {
          for (const r of suiteRows) {
            const res = r0.results && r0.results[r.name];
            perEngine[e.id][r.id] = res ? { ...res, mem: r0.mem } : { error: 'row not produced' };
          }
        }
      }
    } else {
      for (const r of suiteRows) {
        for (const e of engines) {
          const pinned = baseline ? pinnedFromBaseline(baseline, r.suite, r.name) : null;
          process.stderr.write(`\r[${++counter}/${total}] ${r.id} on ${e.id}   `);
          perEngine[e.id][r.id] = run(suite, r.name, e, args, pinned);
        }
      }
    }
  }
  process.stderr.write('\n');

  const memOn = args.no_mem ? false : (args.mem || engines.some(e => e.kind === 'qjs'));
  const rowMetric = (r, v) => v.score ? 'score' : (r.suite === 'startup' ? 'ms' : 'ns');

  if (args.json) {
    const out = { rows: [], engines: engines.map(e => e.id), mem: memOn };
    for (const r of rows) {
      const o = { id: r.id, suite: r.suite };
      for (const e of engines) {
        const v = perEngine[e.id][r.id] || {};
        o[e.id] = { ns: v.ns, ms: v.ms, score: v.score, iters: v.iters, mem: memOn ? v.mem : undefined, error: v.error };
      }
      out.rows.push(o);
    }
    console.log(JSON.stringify(out, null, 2));
    return;
  }

  for (const e of engines) {
    if (!perEngine[e.id]) continue;
    console.log(`\n[${e.id}]`);
    const idW = Math.max(...rows.map(r => r.id.length));
    const numW = 12;
    console.log(`  ${'row'.padEnd(idW)}  ${'value'.padStart(numW)}`);
    for (const r of rows) {
      const v = perEngine[e.id][r.id] || {};
      const m = rowMetric(r, v);
      if (v.error) console.log(`  ${r.id.padEnd(idW)}  ${'(error: ' + v.error + ')'}`);
      else if (m === 'score') console.log(`  ${r.id.padEnd(idW)}  ${String(v.score).padStart(numW)}`);
      else if (m === 'ms') console.log(`  ${r.id.padEnd(idW)}  ${v.ms != null ? v.ms.toFixed(2) + ' ms' : '-'}`);
      else console.log(`  ${r.id.padEnd(idW)}  ${v.ns != null ? v.ns.toFixed(1) : '-'}  ${v.spread != null ? 'spread=' + v.spread.toFixed(1) + '%' : ''}  iters=${v.iters || '-'}`);
    }
    const scores = rows.map(r => perEngine[e.id][r.id]).filter(v => v && v.score > 0).map(v => v.score);
    if (scores.length > 1) console.log(`  ${'geomean'.padEnd(idW)}  ${geomean(scores).toFixed(1).padStart(numW)}`);
  }

  if (engines.length > 1) {
    console.log(`\n[ratio ${engines[1].id}/${engines[0].id}]`);
    const ref = engines[0].id;
    let ratios = [];
    for (const r of rows) {
      const a = perEngine[ref][r.id], b = perEngine[engines[1].id][r.id];
      const m = rowMetric(r, a || {});
      const av = a && a[m], bv = b && b[m];
      if (av > 0 && bv > 0) {
        const r0 = bv / av;
        ratios.push({ id: r.id, ratio: r0 });
        console.log(`  ${r.id.padEnd(40)}  ${r0.toFixed(3)}x`);
      }
    }
    if (ratios.length) console.log(`  ${'geomean'.padEnd(40)}  ${geomean(ratios.map(r => r.ratio)).toFixed(3)}x`);
  }

  if (memOn) {
    console.log(`\n[memory]`);
    for (const e of engines) {
      const first = rows.map(r => perEngine[e.id][r.id]).find(v => v && v.mem);
      if (!first) continue;
      const m = first.mem;
      console.log(`  ${e.id.padEnd(10)}  malloc=${(m.mallocSize/1024).toFixed(0)}KB  obj=${m.objCount}  prop=${m.propCount}  shape=${m.shapeCount}  str=${m.strCount}  atom=${m.atomCount}`);
    }
  }

  if (args.save) {
    const out = { rows: [], engines: engines.map(e => ({ id: e.id, bin: e.bin })) };
    for (const r of rows) {
      const o = { id: r.id, suite: r.suite, name: r.name, pinned_iters: undefined };
      for (const e of engines) {
        const v = perEngine[e.id][r.id] || {};
        o[e.id] = { ns: v.ns, ms: v.ms, score: v.score, iters: v.iters };
        if (v.iters) o.pinned_iters = v.iters;
      }
      out.rows.push(o);
    }
    const p = typeof args.save === 'string' ? args.save : join(HERE, 'baseline.json');
    writeFileSync(p, JSON.stringify(out, null, 2));
    console.error(`\nbaseline written: ${p}`);
  }

  if (args.against) {
    const p = typeof args.against === 'string' ? args.against : join(HERE, 'baseline.json');
    if (!baseline) fail(`baseline ${p} not found`);
    let regressions = 0;
    for (const r of rows) {
      for (const e of engines) {
        const b = (baseline.rows || []).find(br => br.id === r.id);
        if (!b) continue;
        const cur = perEngine[e.id][r.id];
        const m = rowMetric(r, cur || {});
        const bMetric = m === 'score' ? (b[e.id] || {}).score : (m === 'ms' ? (b[e.id] || {}).ms : (b[e.id] || {}).ns);
        const curMetricVal = m === 'score' ? cur.score : (m === 'ms' ? cur.ms : cur.ns);
        if (bMetric > 0 && curMetricVal > 0) {
          const r0 = curMetricVal / bMetric;
          if (m === 'score' ? r0 < 0.97 : r0 > 1.05) {
            console.error(`REGRESS ${r.id} on ${e.id}: ${bMetric.toFixed(2)} -> ${curMetricVal.toFixed(2)} (${r0.toFixed(3)}x)`);
            regressions++;
          }
        }
      }
    }
    if (regressions > 0) { console.error(`${regressions} regression(s)`); process.exit(1); }
    console.error(`no regressions (${p})`);
  }
}

main().catch(e => { console.error(e); process.exit(1); });