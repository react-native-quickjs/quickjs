#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * postinstall, and why it is a script rather than a shell one-liner.
 *
 * This package is installed in two situations that need opposite behaviour:
 *
 *   1. THIS REPOSITORY, from git. engine/quickjs-ng is a submodule that has to
 *      be initialised and patched, or nothing builds.
 *
 *   2. A CONSUMER'S node_modules, from the npm tarball. That ships
 *      engine/quickjs-rel already patched, there is no submodule, and there is
 *      no git repository.
 *
 * Running the first in the second breaks three ways at once: `git submodule
 * update` with no .git of its own walks UP out of node_modules and operates on
 * the consuming app's repository; engine/patches is not published, so the patch
 * script has nothing to apply; and shelling out to a package manager assumes
 * one the consumer may not use.
 *
 * So detect the situation, and in (2) do nothing at all.
 */

'use strict';

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.join(__dirname, '..');

// engine/patches is absent from the tarball on purpose, and its absence is the
// signal. Checking for .git would be wrong: a consumer who vendors this package
// inside their own repository has a .git above them.
if (!fs.existsSync(path.join(root, 'engine', 'patches'))) {
  process.exit(0);
}

function run(label, command, args) {
  const result = spawnSync(command, args, {
    cwd: root,
    stdio: 'inherit',
    encoding: 'utf8',
  });

  if (result.error || result.status !== 0) {
    console.error(
      `\n[react-native-quickjs] ${label} failed.\n` +
        `  ${command} ${args.join(' ')}\n\n` +
        'The engine sources are not usable until this succeeds. Run it by hand\n' +
        'to see the full error.\n'
    );
    process.exit(1);
  }
}

// --recursive matters: quickjs-ng has submodules of its own for its test
// suites, and a partial checkout fails later in cmake rather than here.
run('submodule checkout', 'git', [
  'submodule',
  'update',
  '--init',
  '--recursive',
]);

// Invoked through node rather than a package manager, so this behaves the same
// under npm, yarn, pnpm and bun.
run('patch application', process.execPath, [
  path.join(root, 'scripts', 'apply-patches.js'),
]);
