/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Generates engine/quickjs-rel -- the patched engine sources that ship to npm.
 *
 *   node scripts/sync-quickjs-rel.js            regenerate
 *   node scripts/sync-quickjs-rel.js --check    exit non-zero if out of date
 *
 * There are two copies of the engine. engine/quickjs-ng is the submodule --
 * upstream, plus whatever engine/patches has been applied to its working tree,
 * and where engine development happens. engine/quickjs-rel is a committed,
 * pre-patched copy: what cmake compiles and what npm packs.
 *
 * The submodule cannot be the thing that ships. Our patches live in its working
 * tree, which `git submodule update` reverts, and npm does not pack submodules
 * at all, so a consumer would get an empty directory. Both failure modes leave
 * an engine that still compiles, so neither announces itself.
 */

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { spawnSync } = require('child_process');

const root = path.join(__dirname, '..');
const src = path.join(root, 'engine', 'quickjs-ng');
const dst = path.join(root, 'engine', 'quickjs-rel');
const patchDir = path.join(root, 'engine', 'patches');

const check = process.argv.includes('--check');

/*
 * The four translation units, every local header they reach, and the licence.
 *
 * This list is the include closure of quickjs.c, libregexp.c, libunicode.c and
 * dtoa.c. Keep it that way.
 *
 * A new `#include "..."` in the engine is a change to THIS ARRAY too, on the
 * same commit. This script projects a fixed list, so a header it does not name
 * is deleted from engine/quickjs-rel as a leftover -- and the failure is a
 * `fatal error: '...' file not found` in the release build, far from the patch
 * that caused it.
 */
const FILES = [
  // translation units
  'quickjs.c',
  'libregexp.c',
  'libunicode.c',
  'dtoa.c',
  // headers
  'builtin-array-fromasync.h',
  'builtin-iterator-zip-keyed.h',
  'builtin-iterator-zip.h',
  'cutils.h',
  'dtoa.h',
  'libregexp-opcode.h',
  'libregexp.h',
  'libunicode-table.h',
  'libunicode.h',
  'list.h',
  'quickjs-atom.h',
  'quickjs-c-atomics.h',
  'quickjs-opcode.h',
  'quickjs.h',
  // provenance
  'LICENSE',
];

/* Proof the patches are applied: JS_GetMallocSize comes from engine/patches/0001
   and exists nowhere upstream. Projecting an unpatched engine would bake it into
   a committed file, where it is far harder to notice than in a submodule. */
const PATCH_MARKER = 'JS_GetMallocSize';

function sha256(buffer) {
  return crypto.createHash('sha256').update(buffer).digest('hex');
}

function fail(message) {
  console.error(`\n[sync-quickjs-rel] ${message}\n`);
  process.exit(1);
}

function submoduleCommit() {
  /*
   * GIT_DIR must be stripped, or `-C src` is silently ignored.
   *
   * `git -C <dir> rev-parse HEAD` resolves against $GIT_DIR when that variable
   * is set, whatever -C says. Git sets GIT_DIR and GIT_INDEX_FILE for every
   * hook it runs, so inside a pre-commit hook this returns the SUPERPROJECT's
   * HEAD instead of the submodule's. The manifest is then written with the
   * wrong commit, and `--check` reports the projection stale with no way to
   * make it fresh.
   *
   * It only bites in a linked worktree, where git sets GIT_DIR to an absolute
   * path; in a normal checkout the variable is unset and `-C` works. That is
   * why it can survive a long time unnoticed.
   */
  const env = { ...process.env };
  delete env.GIT_DIR;
  delete env.GIT_INDEX_FILE;
  delete env.GIT_WORK_TREE;
  const result = spawnSync('git', ['-C', src, 'rev-parse', 'HEAD'], {
    encoding: 'utf8',
    env,
  });
  return result.status === 0 ? result.stdout.trim() : 'unknown';
}

// --- gather ----------------------------------------------------------------

if (!fs.existsSync(path.join(src, 'quickjs.c'))) {
  fail(
    'engine/quickjs-ng is empty.\n' +
      'Run: git submodule update --init --recursive && node scripts/apply-patches.js'
  );
}

const engine = fs.readFileSync(path.join(src, 'quickjs.c'), 'utf8');
if (!engine.includes(PATCH_MARKER)) {
  fail(
    'engine/quickjs-ng has no patches applied.\n' +
      'Refusing to generate engine/quickjs-rel from an unpatched engine.\n' +
      'Run: node scripts/apply-patches.js'
  );
}

/** @type {Record<string, string>} */
const hashes = {};
/** @type {Record<string, Buffer>} */
const contents = {};

for (const name of FILES) {
  const file = path.join(src, name);
  if (!fs.existsSync(file)) {
    fail(
      `engine/quickjs-ng/${name} does not exist.\n` +
        'If upstream renamed or removed it, update FILES in this script.'
    );
  }
  const buffer = fs.readFileSync(file);
  contents[name] = buffer;
  hashes[name] = sha256(buffer);
}

// The patch stack is recorded by content, not by filename. A patch edited in
// place without being renamed is the case a filename list would miss.
const patches = fs.existsSync(patchDir)
  ? fs
      .readdirSync(patchDir)
      .filter((name) => name.endsWith('.patch'))
      .sort()
      .map((name) => ({
        name,
        sha256: sha256(fs.readFileSync(path.join(patchDir, name))),
      }))
  : [];

const manifest = {
  _comment:
    'Generated by scripts/sync-quickjs-rel.js. Do not edit. These files are ' +
    'engine/quickjs-ng at the commit below, with the patches below applied.',
  upstreamCommit: submoduleCommit(),
  patches,
  files: hashes,
};

// No timestamp, deliberately: the same inputs must produce a byte-identical
// manifest, or every regeneration shows up as a diff and the signal is lost.
const manifestText = JSON.stringify(manifest, null, 2) + '\n';

// --- check -----------------------------------------------------------------

if (check) {
  const manifestPath = path.join(dst, 'MANIFEST.json');
  if (!fs.existsSync(manifestPath)) {
    fail(
      'engine/quickjs-rel/MANIFEST.json is missing.\n' +
        'Run: node scripts/sync-quickjs-rel.js'
    );
  }

  const stale = [];
  if (fs.readFileSync(manifestPath, 'utf8') !== manifestText) {
    stale.push('MANIFEST.json');
  }
  for (const name of FILES) {
    const file = path.join(dst, name);
    if (!fs.existsSync(file) || sha256(fs.readFileSync(file)) !== hashes[name]) {
      stale.push(name);
    }
  }

  if (stale.length > 0) {
    fail(
      'engine/quickjs-rel is out of date with engine/quickjs-ng + engine/patches.\n\n' +
        stale.map((name) => `  ${name}`).join('\n') +
        '\n\nRun: node scripts/sync-quickjs-rel.js'
    );
  }

  console.log(
    `[sync-quickjs-rel] up to date (${FILES.length} files, ${patches.length} patches)`
  );
  process.exit(0);
}

// --- write -----------------------------------------------------------------

fs.mkdirSync(dst, { recursive: true });

let changed = 0;
for (const name of FILES) {
  const file = path.join(dst, name);
  const existing = fs.existsSync(file) ? fs.readFileSync(file) : null;
  if (existing === null || !existing.equals(contents[name])) {
    fs.writeFileSync(file, contents[name]);
    changed += 1;
  }
}

const manifestPath = path.join(dst, 'MANIFEST.json');
const existingManifest = fs.existsSync(manifestPath)
  ? fs.readFileSync(manifestPath, 'utf8')
  : null;
if (existingManifest !== manifestText) {
  fs.writeFileSync(manifestPath, manifestText);
  changed += 1;
}

// Anything in engine/quickjs-rel that FILES no longer lists is a leftover from
// an earlier upstream layout. Removing it matters: a stale header sitting on
// the include path shadows nothing today but will silently win over a renamed
// one later.
const known = new Set([...FILES, 'MANIFEST.json']);
for (const name of fs.readdirSync(dst)) {
  if (!known.has(name)) {
    fs.rmSync(path.join(dst, name), { recursive: true, force: true });
    console.log(`[sync-quickjs-rel] removed stale ${name}`);
    changed += 1;
  }
}

console.log(
  changed === 0
    ? `[sync-quickjs-rel] already up to date (${FILES.length} files)`
    : `[sync-quickjs-rel] wrote ${changed} file(s) to engine/quickjs-rel`
);
