#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The engine is engine/quickjs-ng, a submodule pointing at our fork of quickjs-ng (engine/README.md). Every consuming app compiles it
 * from the files package.json "files" lists under engine/quickjs-ng/, so this fails before a commit, a pack or a release when:
 *
 *   - the submodule is not checked out;
 *   - it is not at the commit the main repository records (in its index: a commit that moves the pointer checks the new one);
 *   - a tracked engine file has an uncommitted edit (an engine change is a commit in the fork, never a working-tree edit);
 *   - an engine file the build needs is missing, or is not tracked by the fork;
 *   - (pack check) the tarball npm would publish lacks one of those files, or carries another engine file.
 *
 * In an extracted tarball (no git), only the file check runs. --no-pack skips the pack check (the commit hook).
 */

'use strict';

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.join(__dirname, '..');
const ENGINE = 'engine/quickjs-ng';
const noPack = process.argv.includes('--no-pack');

let failed = false;
function fail(msg) {
  console.error(`[check-engine] ${msg}`);
  failed = true;
}
function git(args, cwd = root) {
  return spawnSync('git', args, { cwd, encoding: 'utf8' });
}

const pkg = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
const needed = pkg.files.filter((f) => f.startsWith(`${ENGINE}/`));
if (needed.length === 0) fail(`package.json "files" lists no ${ENGINE}/ file`);

for (const f of needed) {
  if (!fs.existsSync(path.join(root, f))) fail(`${f} is missing (run: git submodule update --init --recursive)`);
}

const inRepo = fs.existsSync(path.join(root, '.gitmodules')) && git(['rev-parse', '--git-dir']).status === 0;
if (inRepo && !failed) {
  const st = git(['submodule', 'status', ENGINE]);
  const line = (st.stdout || '').trim();
  if (st.status !== 0 || line === '') fail(`git submodule status ${ENGINE} failed: ${st.stderr}`);
  else if (line.startsWith('-')) fail(`${ENGINE} is not checked out (run: git submodule update --init --recursive)`);
  else if (line.startsWith('+')) fail(`${ENGINE} is not at the commit the repository records:\n  ${line}\n  commit the new pointer (git add ${ENGINE}) or check out the recorded commit`);
  else if (line.startsWith('U')) fail(`${ENGINE} has a merge conflict in its pointer: ${line}`);

  const eng = path.join(root, ENGINE);
  const dirty = git(['status', '--porcelain', '--untracked-files=no'], eng);
  if (dirty.status !== 0) fail(`git status in ${ENGINE} failed: ${dirty.stderr}`);
  else if (dirty.stdout.trim() !== '')
    fail(`${ENGINE} has uncommitted edits (an engine change is a commit in the fork, see engine/README.md):\n${dirty.stdout}`);

  const tracked = new Set(git(['ls-files'], eng).stdout.split('\n'));
  for (const f of needed) {
    const rel = f.slice(ENGINE.length + 1);
    if (!tracked.has(rel)) fail(`${f} is not tracked by the fork: it would ship from this working tree only`);
  }
  if (!failed) console.log(`[check-engine] ${line}`);
}

if (!noPack && !failed) {
  const npm = process.platform === 'win32' ? 'npm.cmd' : 'npm';
  const r = spawnSync(npm, ['pack', '--dry-run', '--json', '--ignore-scripts'], { cwd: root, encoding: 'utf8', maxBuffer: 64 << 20 });
  if (r.status !== 0) fail(`npm pack --dry-run failed: ${r.stderr}`);
  else {
    const packed = new Set(JSON.parse(r.stdout)[0].files.map((x) => x.path));
    for (const f of needed) if (!packed.has(f)) fail(`${f} is not in the tarball npm would publish`);
    for (const f of packed) if (f.startsWith(`${ENGINE}/`) && !needed.includes(f)) fail(`${f} is in the tarball but not in the engine file list`);
    if (!failed) console.log(`[check-engine] tarball carries the ${needed.length} engine files`);
  }
}

process.exit(failed ? 1 : 0);
