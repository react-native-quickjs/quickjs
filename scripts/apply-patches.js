#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Applies our patches to the vendored engine.
 *
 * engine/quickjs-ng tracks upstream, so local changes live in engine/patches
 * rather than as commits in a fork. Anything that has to survive a submodule
 * bump belongs there, with a header explaining what it does and why.
 *
 * Idempotent: safe to run repeatedly, and safe to run after
 * `git submodule update` has reset the working tree.
 *
 *   node scripts/apply-patches.js            apply (default)
 *   node scripts/apply-patches.js --check    exit non-zero if not applied
 *   node scripts/apply-patches.js --reverse  remove them again
 */

'use strict';

const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawnSync } = require('child_process');

const root = path.join(__dirname, '..');
const submodule = path.join(root, 'engine', 'quickjs-ng');
const patchDir = path.join(root, 'engine', 'patches');
const readmePath = path.join(patchDir, 'README.md');

/*
 * The one patch allowed to change the bytecode version, and the only patch
 * that ever should. See the "no patch bumps the bytecode version" section of
 * engine/patches/README.md.
 */
const BC_PATCH = '9999-bc-version-bump.patch';

const mode = process.argv.includes('--check')
  ? 'check'
  : process.argv.includes('--reverse')
    ? 'reverse'
    : 'apply';

function fail(message) {
  console.error(`\n[apply-patches] ${message}\n`);
  process.exit(1);
}

/*
 * Git sets GIT_DIR and GIT_INDEX_FILE for every hook it runs, and when they
 * are set `git -C <dir>` resolves against them instead of the directory named.
 * Inside a pre-commit hook that means every command below would silently
 * operate on the superproject rather than the submodule.
 */
function git(args, options = {}) {
  const env = { ...process.env };
  delete env.GIT_DIR;
  delete env.GIT_INDEX_FILE;
  delete env.GIT_WORK_TREE;
  return spawnSync('git', ['-C', submodule, ...args], {
    encoding: 'utf8',
    env,
    // quickjs.c alone is over a megabyte, and Node's default maxBuffer is
    // exactly one. Past it the child is killed and `status` comes back null
    // with truncated output -- which reads like a git failure but is not.
    maxBuffer: 64 * 1024 * 1024,
    ...options,
  });
}

function patchNames() {
  if (!fs.existsSync(patchDir)) return [];
  return fs
    .readdirSync(patchDir)
    .filter((name) => name.endsWith('.patch'))
    .sort();
}

/*
 * The files any patch touches, as paths relative to the submodule root.
 */
function touchedFiles(patches) {
  const files = new Set();
  for (const patch of patches) {
    const body = fs.readFileSync(path.join(patchDir, patch), 'utf8');
    for (const m of body.matchAll(/^\+\+\+ b\/(.+)$/gm)) files.add(m[1].trim());
  }
  return [...files].sort();
}

/*
 * Builds "upstream plus the whole series" in a scratch directory and returns
 * its path, or null if some patch does not apply.
 *
 * The series is stacked: each patch is cut against the tree with the previous
 * ones already applied. That makes a per-patch check meaningless -- reversing
 * patch 1 alone against the finished tree fails, because patch 2 changed the
 * lines around it. The only question worth asking is whether the working tree
 * equals upstream with every patch applied in order, so that is what we build.
 */
function buildExpected(patches, files) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'rnqjs-patches-'));
  for (const file of files) {
    const blob = git(['show', `HEAD:${file}`], { encoding: 'buffer' });
    if (blob.status !== 0) {
      fs.rmSync(dir, { recursive: true, force: true });
      fail(`engine/quickjs-ng has no ${file} at HEAD; the submodule may have moved.`);
    }
    fs.mkdirSync(path.join(dir, path.dirname(file)), { recursive: true });
    fs.writeFileSync(path.join(dir, file), blob.stdout);
  }

  for (const patch of patches) {
    const result = spawnSync('git', ['apply', path.join(patchDir, patch)], {
      cwd: dir,
      encoding: 'utf8',
    });
    if (result.status !== 0) {
      fs.rmSync(dir, { recursive: true, force: true });
      return { dir: null, failed: patch, stderr: result.stderr };
    }
  }
  return { dir, failed: null, stderr: '' };
}

/* Which of `files` in the submodule working tree differ from `dir`. */
function diverged(dir, files) {
  return files.filter((file) => {
    const a = path.join(dir, file);
    const b = path.join(submodule, file);
    if (!fs.existsSync(b)) return true;
    return !fs.readFileSync(a).equals(fs.readFileSync(b));
  });
}

/* Whether the working tree is still exactly upstream for these files. */
function isPristine(files) {
  return files.every((file) => {
    const blob = git(['show', `HEAD:${file}`], { encoding: 'buffer' });
    if (blob.status !== 0) return false;
    const onDisk = path.join(submodule, file);
    return fs.existsSync(onDisk) && blob.stdout.equals(fs.readFileSync(onDisk));
  });
}

// --- the two series rules --------------------------------------------------

/*
 * Every patch has a row in the README, and every row names a real patch.
 *
 * The table is how someone understands this fork without opening a single
 * .patch file, so it is worth failing a build over. A table nobody checks
 * drifts within a month.
 */
function checkReadmeRows(patches) {
  if (!fs.existsSync(readmePath)) {
    fail('engine/patches/README.md is missing. Every patch needs a row in it.');
  }
  const readme = fs.readFileSync(readmePath, 'utf8');
  const problems = [];

  for (const patch of patches) {
    const number = patch.slice(0, patch.indexOf('-'));
    if (!new RegExp(`\`${number}\``).test(readme)) {
      problems.push(`  ${patch} has no row in README.md`);
    }
  }

  const rows = [...readme.matchAll(/^\|\s*`(\d{4})`\s*\|/gm)].map((m) => m[1]);
  for (const number of rows) {
    if (!patches.some((p) => p.startsWith(`${number}-`))) {
      problems.push(`  README.md has a row for ${number}, but no such patch`);
    }
  }

  if (problems.length > 0) {
    fail(`engine/patches/README.md does not match the patches:\n\n${problems.join('\n')}`);
  }
}

/*
 * No patch except the tail patch touches BC_VERSION.
 *
 * Without this rule every patch that changes the bytecode format bumps the
 * version itself, so any two of them conflict over the same line for reasons
 * unrelated to what either one does -- and the number ends up defined several
 * times in one file, with whichever comes first winning.
 */
function checkBytecodeRule(patches) {
  const offenders = [];
  let declaresBump = false;

  for (const patch of patches) {
    const body = fs.readFileSync(path.join(patchDir, patch), 'utf8');
    const touches = body
      .split('\n')
      .some((line) => /^[+-]/.test(line) && !/^(\+\+\+|---)/.test(line) && line.includes('BC_VERSION'));

    if (touches && patch !== BC_PATCH) {
      offenders.push(`  ${patch} changes BC_VERSION; only ${BC_PATCH} may`);
    }
    if (/Needs a bytecode version bump\s*\n\s*Yes/i.test(body)) {
      declaresBump = true;
    }
  }

  if (declaresBump && !patches.includes(BC_PATCH)) {
    offenders.push(
      `  a patch declares it needs a bytecode version bump, but ${BC_PATCH} is missing`
    );
  }

  if (offenders.length > 0) {
    fail(`the bytecode version rule is broken:\n\n${offenders.join('\n')}`);
  }
}

// --- run -------------------------------------------------------------------

if (!fs.existsSync(path.join(submodule, 'quickjs.c'))) {
  fail(
    'engine/quickjs-ng is empty.\n' +
      'Run: git submodule update --init --recursive'
  );
}

const patches = patchNames();
if (patches.length === 0) {
  console.log('[apply-patches] no patches in engine/patches');
  process.exit(0);
}

/* --reverse is the escape hatch out of a half-applied tree, so it must work
   even when the series rules are broken -- otherwise a bad patch can wedge
   the submodule with no supported way back. */
if (mode !== 'reverse') {
  checkReadmeRows(patches);
  checkBytecodeRule(patches);
}

const files = touchedFiles(patches);
const expected = buildExpected(patches, files);

if (mode === 'check') {
  if (expected.dir === null) {
    fail(
      `${expected.failed} does not apply to upstream plus the patches before it.\n` +
        expected.stderr
    );
  }
  const stale = diverged(expected.dir, files);
  fs.rmSync(expected.dir, { recursive: true, force: true });
  if (stale.length > 0) {
    fail(
      'engine/quickjs-ng does not match upstream plus engine/patches.\n\n' +
        stale.map((name) => `  ${name}`).join('\n') +
        '\n\nRun: node scripts/apply-patches.js'
    );
  }
  console.log(`[apply-patches] all ${patches.length} patches applied`);
  process.exit(0);
}

if (mode === 'reverse') {
  for (const patch of [...patches].reverse()) {
    const result = git(['apply', '--reverse', path.join(patchDir, patch)]);
    if (result.status !== 0) {
      fail(`could not reverse ${patch}\n${result.stderr}`);
    }
  }
  if (expected.dir) fs.rmSync(expected.dir, { recursive: true, force: true });
  console.log(`[apply-patches] reversed ${patches.length} patch(es)`);
  process.exit(0);
}

// apply
if (expected.dir === null) {
  fail(
    `${expected.failed} does not apply to upstream plus the patches before it.\n` +
      expected.stderr
  );
}

if (diverged(expected.dir, files).length === 0) {
  fs.rmSync(expected.dir, { recursive: true, force: true });
  console.log(`[apply-patches] already applied (${patches.length} patches)`);
  process.exit(0);
}

if (!isPristine(files)) {
  fs.rmSync(expected.dir, { recursive: true, force: true });
  fail(
    'engine/quickjs-ng is neither upstream nor fully patched.\n' +
      'Run: node scripts/apply-patches.js --reverse\n' +
      '  or: git submodule update --force --recursive'
  );
}

for (const patch of patches) {
  const result = git(['apply', path.join(patchDir, patch)]);
  if (result.status !== 0) fail(`could not apply ${patch}\n${result.stderr}`);
}
fs.rmSync(expected.dir, { recursive: true, force: true });
console.log(`[apply-patches] applied ${patches.length} patch(es)`);
