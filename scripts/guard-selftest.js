#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Proves the engine guards can fail.
 *
 * A check that cannot go red is not a guard, and the way that happens is never
 * dramatic: a path stops matching, a rule is checked in the wrong mode, and
 * the command keeps printing a reassuring line. Each case below breaks one
 * thing on purpose, asserts the guard notices, and puts it back.
 *
 *   node scripts/guard-selftest.js
 */

'use strict';

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.join(__dirname, '..');
const engineFile = path.join(root, 'engine', 'quickjs-ng', 'quickjs.h');

function run(script, args = []) {
  return spawnSync(process.execPath, [path.join(root, 'scripts', script), ...args], {
    cwd: root,
    encoding: 'utf8',
  }).status;
}
function git(args) {
  const r = spawnSync('git', args, { cwd: root, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`git ${args.join(' ')}: ${r.stderr}`);
  return r.stdout.trim();
}

let failures = 0;

/* Break one thing, assert the guard goes red, put it back, assert it goes green. */
function expectRed(label, script, args, breakIt, restore) {
  let red;
  try {
    breakIt();
    red = run(script, args) !== 0;
  } finally {
    restore();
  }
  const green = run(script, args) === 0;
  const ok = red && green;
  console.log(`${ok ? 'ok  ' : 'FAIL'}  ${label}${ok ? '' : `  (red=${red}, green=${green})`}`);
  if (!ok) failures += 1;
}

/* A file restored from a saved copy, whatever the check did in between. */
function withSaved(file, mutate) {
  const saved = fs.readFileSync(file);
  return [() => mutate(saved), () => fs.writeFileSync(file, saved)];
}

expectRed(
  'check-engine notices an uncommitted engine edit',
  'check-engine.js',
  ['--no-pack'],
  ...withSaved(engineFile, () => fs.appendFileSync(engineFile, '\n/* selftest */\n'))
);

{
  const f = path.join(root, 'engine', 'quickjs-ng', 'dtoa.h');
  const away = `${f}.selftest`;
  expectRed(
    'check-engine notices a missing engine file',
    'check-engine.js',
    [],
    () => fs.renameSync(f, away),
    () => fs.renameSync(away, f)
  );
}

/* The pointer the main repository records (its index entry) moved away from the submodule's HEAD; only the index entry changes. */
{
  const head = git(['-C', 'engine/quickjs-ng', 'rev-parse', 'HEAD']);
  const other = git(['-C', 'engine/quickjs-ng', 'rev-parse', 'HEAD~1']);
  const recorded = git(['ls-files', '-s', 'engine/quickjs-ng']).split(/\s+/)[1];
  expectRed(
    'check-engine notices the submodule off the recorded commit',
    'check-engine.js',
    ['--no-pack'],
    () => git(['update-index', '--cacheinfo', `160000,${other},engine/quickjs-ng`]),
    () => git(['update-index', '--cacheinfo', `160000,${recorded},engine/quickjs-ng`])
  );
  if (git(['ls-files', '-s', 'engine/quickjs-ng']).split(/\s+/)[1] !== recorded || head !== recorded)
    console.log(`note: recorded pointer ${recorded}, submodule HEAD ${head}`);
}

console.log(
  failures === 0
    ? '\n[guard-selftest] all guards can fail, and recover'
    : `\n[guard-selftest] ${failures} guard(s) did not behave`
);
process.exit(failures === 0 ? 0 : 1);
