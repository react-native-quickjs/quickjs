#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The engine sources are not optional: every consuming app compiles them. Fail
 * the pack rather than publish a package that cannot build.
 *
 * What ships is engine/quickjs-rel, the committed projection, so the check is
 * on that. The submodule is checked too when there is one, because a projection
 * generated from a submodule at the wrong commit is the engine we did not mean
 * to publish.
 */

'use strict';

const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

const root = path.join(__dirname, '..');

if (!fs.existsSync(path.join(root, 'engine', 'quickjs-rel', 'quickjs.c'))) {
  console.error(
    '\n[react-native-quickjs] engine/quickjs-rel is missing.\n' +
      'Run: node scripts/sync-quickjs-rel.js\n'
  );
  process.exit(1);
}

try {
  const status = execSync('git submodule status --cached engine/quickjs-ng', {
    cwd: root,
    encoding: 'utf8',
  }).trim();

  // '+' means the submodule sits at a different commit than the one recorded;
  // '-' means it is not checked out. Our patches change the working tree only,
  // so neither should appear.
  if (status.startsWith('+') || status.startsWith('-')) {
    console.error(
      `\n[react-native-quickjs] engine/quickjs-ng is not at its recorded commit:\n  ${status}\n`
    );
    process.exit(1);
  }

  const patchDir = path.join(root, 'engine', 'patches');
  const patches = fs.existsSync(patchDir)
    ? fs.readdirSync(patchDir).filter((f) => f.endsWith('.patch')).length
    : 0;

  console.log(`[react-native-quickjs] quickjs-ng ${status} + ${patches} patch(es)`);
} catch {
  // Not a git checkout, e.g. packing from an extracted tarball. The file check
  // above is the part that matters there.
}
