#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..', '..', '..');
const FIXTURES = join(HERE, 'fixtures');

const argv = process.argv.slice(2);
const opt = (n, d = null) => { const i = argv.indexOf(n); return i >= 0 && argv[i + 1] != null ? argv[i + 1] : d; };
const flag = (n) => argv.includes(n);

if (flag('--help') || flag('-h')) {
  console.log('usage: build-octane.mjs [--out DIR] [--check] [--quiet] [--rows a,b]');
  process.exit(0);
}

const ROWS = {
  richards:     ['richards.js'],
  deltablue:    ['deltablue.js'],
  crypto:       ['crypto.js'],
  raytrace:     ['raytrace.js'],
  earleyboyer:  ['earley-boyer.js'],
  regexp:       ['regexp.js'],
  splay:        ['splay.js'],
  navierstokes: ['navier-stokes.js'],
  pdfjs:        ['pdfjs.js'],
  mandreel:     ['mandreel.js'],
  gbemu:        ['gbemu-part1.js', 'gbemu-part2.js'],
  box2d:        ['box2d.js'],
  typescript:   ['typescript.js', 'typescript-input.js', 'typescript-compiler.js'],
};

const OUT = resolve(opt('--out', join(HERE, 'corpus')));
const WANT = (opt('--rows', null) || Object.keys(ROWS).join(',')).split(',').map(s => s.trim()).filter(Boolean);
for (const r of WANT) if (!ROWS[r]) { console.error(`unknown row ${r}`); process.exit(2); }

const base = readFileSync(join(FIXTURES, 'base.js'), 'utf8');
const tail = `\nvar success = true;
function PrintResult(name, result) { print(name + ': ' + result); }
function PrintError(name, error) { PrintResult(name, error); success = false; }
function PrintScore(score) { if (success) print('Score: ' + score); }
BenchmarkSuite.config.doWarmup = undefined;
BenchmarkSuite.config.doDeterministic = undefined;
BenchmarkSuite.RunSuites({ NotifyResult: PrintResult, NotifyError: PrintError, NotifyScore: PrintScore });
`;

const md5 = (b) => createHash('md5').update(b).digest('hex');
const manifest = { built: new Date().toISOString(), rows: [] };

if (flag('--check')) {
  const mPath = join(OUT, 'MANIFEST.json');
  if (!existsSync(mPath)) { console.error('no MANIFEST.json at', OUT); process.exit(3); }
  const cur = JSON.parse(readFileSync(mPath, 'utf8'));
  for (const r of WANT) {
    const want = manifest.rows.find(x => x.row === r) || { row: r };
    if (!cur.rows || !cur.rows.find(x => x.row === r)) { console.error(`missing row ${r} in ${OUT}`); process.exit(3); }
  }
  if (!flag('--quiet')) console.log(`corpus OK at ${OUT}`);
  process.exit(0);
}

mkdirSync(OUT, { recursive: true });
for (const name of WANT) {
  const parts = ROWS[name];
  const partsText = parts.map(p => readFileSync(join(FIXTURES, p), 'utf8')).join('\n\n');
  const src = base + '\n' + partsText + tail;
  const out = join(OUT, `${name}.js`);
  writeFileSync(out, src);
  const row = { row: name, parts, bytes: src.length, md5: md5(src) };
  manifest.rows.push(row);
  if (!flag('--quiet')) console.log(`  ${name.padEnd(14)} ${src.length} B`);
}
writeFileSync(join(OUT, 'MANIFEST.json'), JSON.stringify(manifest, null, 2));
if (!flag('--quiet')) console.log(`wrote ${WANT.length} row(s) -> ${OUT}`);