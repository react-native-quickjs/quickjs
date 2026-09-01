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

/* Break one thing, assert the guard goes red, put it back, assert it goes green. */
function expectRed(label, script, breakIt, restore) {
  let red;
  try {
    breakIt();
    red = run(script, ['--check']) !== 0;
  } finally {
    restore();
  }
  const green = run(script, ['--check']) === 0;
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
  'apply-patches notices an edited engine',
  'apply-patches.js',
  ...withSaved(engineFile, () => fs.appendFileSync(engineFile, '\n/* selftest */\n'))
);

expectRed(
  'apply-patches notices a patch missing from the table',
  'apply-patches.js',
  ...withSaved(readme, (saved) =>
    fs.writeFileSync(readme, saved.toString().replace(/^\| `0003`.*\n/m, ''))
  )
);

expectRed(
  'apply-patches notices a stray BC_VERSION change',
  'apply-patches.js',
  ...withSaved(path.join(patchDir, '0001-runtime-malloc-size-accessor.patch'), (saved) =>
    fs.writeFileSync(
      path.join(patchDir, '0001-runtime-malloc-size-accessor.patch'),
      saved + '\n+#define BC_VERSION 99\n'
    )
  )
);

expectRed(
  'sync-quickjs-rel notices a stale projection',
  'sync-quickjs-rel.js',
  ...withSaved(projection, () => fs.appendFileSync(projection, '\n/* selftest */\n'))
);

console.log(
  failures === 0
    ? '\n[guard-selftest] all guards can fail, and recover'
    : `\n[guard-selftest] ${failures} guard(s) did not behave`
);
process.exit(failures === 0 ? 0 : 1);
