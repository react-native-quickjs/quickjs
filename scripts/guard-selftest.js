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
const patchDir = path.join(root, 'engine', 'patches');
const readme = path.join(patchDir, 'README.md');
const engineFile = path.join(root, 'engine', 'quickjs-ng', 'quickjs.h');
const projection = path.join(root, 'engine', 'quickjs-rel', 'quickjs.h');

function run(script, args = []) {
  return spawnSync(process.execPath, [path.join(root, 'scripts', script), ...args], {
    cwd: root,
    encoding: 'utf8',
  }).status;
}

let failures = 0;

function expectRed(name, breakIt, restore) {
  let status;
  try {
    breakIt();
    status = run(...name.command);
  } finally {
    restore();
  }
  const green = run(...name.command) === 0;
  const ok = status !== 0 && green;
  console.log(
    `${ok ? 'ok  ' : 'FAIL'}  ${name.label}` +
      (ok ? '' : `  (red=${status !== 0}, green-after-restore=${green})`)
  );
  if (!ok) failures += 1;
}

const applyCheck = { label: '', command: ['apply-patches.js', ['--check']] };
const syncCheck = { label: '', command: ['sync-quickjs-rel.js', ['--check']] };

// 1. The submodule drifting away from upstream-plus-patches.
{
  const saved = fs.readFileSync(engineFile);
  expectRed(
    { ...applyCheck, label: 'apply-patches notices an edited engine' },
    () => fs.appendFileSync(engineFile, '\n/* selftest */\n'),
    () => fs.writeFileSync(engineFile, saved)
  );
}

// 2. A patch with no row in the table.
{
  const saved = fs.readFileSync(readme, 'utf8');
  expectRed(
    { ...applyCheck, label: 'apply-patches notices a patch missing from the table' },
    () => fs.writeFileSync(readme, saved.replace(/^\| `0003`.*\n/m, '')),
    () => fs.writeFileSync(readme, saved)
  );
}

// 3. A patch other than the tail one touching the bytecode version.
{
  const victim = path.join(patchDir, '0001-runtime-malloc-size-accessor.patch');
  const saved = fs.readFileSync(victim, 'utf8');
  expectRed(
    { ...applyCheck, label: 'apply-patches notices a stray BC_VERSION change' },
    () => fs.writeFileSync(victim, saved + '\n+#define BC_VERSION 99\n'),
    () => fs.writeFileSync(victim, saved)
  );
}

// 4. The shipped projection drifting from the submodule.
{
  const saved = fs.readFileSync(projection);
  expectRed(
    { ...syncCheck, label: 'sync-quickjs-rel notices a stale projection' },
    () => fs.appendFileSync(projection, '\n/* selftest */\n'),
    () => fs.writeFileSync(projection, saved)
  );
}

console.log(
  failures === 0
    ? '\n[guard-selftest] all guards can fail, and recover'
    : `\n[guard-selftest] ${failures} guard(s) did not behave`
);
process.exit(failures === 0 ? 0 : 1);
