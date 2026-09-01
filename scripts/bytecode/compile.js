#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Compiles a Metro bundle to QuickJS bytecode, in place. Run from the Xcode
 * bundle phase and the Gradle bundle task; see README.md.
 *
 *   node scripts/bytecode/compile.js <bundle>
 *
 * Bytecode is only loadable by the engine build that produced it, so the
 * compiler is pinned to the engine by BC_VERSION. A mismatch stops the build:
 * shipping a bundle this engine cannot read fails at launch, on a user's
 * device, with nothing to point at.
 *
 * Missing compiler for the host is not an error. The bundle is left as
 * JavaScript, which runs -- just without the startup that bytecode buys.
 */

'use strict';

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const ROOT = path.join(__dirname, '..', '..');
const COMPILERS = path.join(ROOT, 'bin', 'qjsc');

/** The BC_VERSION the shipped engine sources define. */
function engineBytecodeVersion() {
  const engine = fs.readFileSync(path.join(ROOT, 'engine', 'quickjs-rel', 'quickjs.c'), 'utf8');
  const found = /^#define BC_VERSION (\d+)/m.exec(engine);
  if (!found) throw new Error('no BC_VERSION in engine/quickjs-rel/quickjs.c');
  return Number(found[1]);
}

/** e.g. darwin-arm64. Matches the directory names the CI workflow writes. */
function hostPlatform() {
  return `${process.platform}-${process.arch}`;
}

function compilerPath() {
  const name = process.platform === 'win32' ? 'qjsc.exe' : 'qjsc';
  return path.join(COMPILERS, hostPlatform(), name);
}

function skip(reason) {
  console.log(`[quickjs] ${reason}; leaving the bundle as JavaScript.`);
  process.exit(0);
}

function main(bundle) {
  if (!bundle) {
    console.error('usage: compile.js <bundle>');
    process.exit(2);
  }
  if (!fs.existsSync(bundle)) skip(`${bundle} does not exist`);

  const compiler = compilerPath();
  if (!fs.existsSync(compiler)) skip(`no bytecode compiler for ${hostPlatform()}`);

  // Zipped artifacts lose the executable bit, so a compiler can arrive in the
  // repository unrunnable. Restoring it here is cheaper than the EACCES this
  // would otherwise become.
  try {
    fs.accessSync(compiler, fs.constants.X_OK);
  } catch {
    fs.chmodSync(compiler, 0o755);
  }

  const manifestPath = path.join(COMPILERS, 'manifest.json');
  const manifest = fs.existsSync(manifestPath)
    ? JSON.parse(fs.readFileSync(manifestPath, 'utf8'))
    : {};
  const engineVersion = engineBytecodeVersion();

  if (manifest.bcVersion !== engineVersion) {
    console.error(
      `\n[quickjs] the bytecode compiler is stale.\n` +
        `  compiler built for BC_VERSION ${manifest.bcVersion}\n` +
        `  engine expects BC_VERSION    ${engineVersion}\n\n` +
        `Bytecode from this compiler would not load. Rebuild the compilers\n` +
        `(the "bytecode compilers" workflow) or unset RNQJS_BYTECODE.\n`
    );
    process.exit(1);
  }

  const compiled = `${bundle}.qbc`;
  const args = process.env.RNQJS_BYTECODE_KEEP_SOURCE ? [] : ['--strip-source'];
  execFileSync(compiler, [...args, bundle, compiled], { stdio: 'inherit' });
  fs.renameSync(compiled, bundle);

  const size = fs.statSync(bundle).size;
  console.log(`[quickjs] compiled ${path.basename(bundle)} to bytecode (${size} bytes)`);
}

main(process.argv[2]);
