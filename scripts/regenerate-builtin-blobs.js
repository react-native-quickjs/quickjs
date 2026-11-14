/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Rebuilds QuickJS's embedded bytecode in engine/quickjs-ng with the qjsc built
 * from it, once per VM build, and smoke-tests each result: the stack build
 * writes builtin-*.h, the register build (QJS_REGVM=1, clang) builtin-*.regvm.h.
 * A blob is loadable only by the build and BC_VERSION it was compiled with, so
 * run this after a bytecode-format change or an upstream sync, then commit the
 * changed blobs in the fork (see engine/README.md).
 */

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const {spawnSync} = require('child_process');

const root = path.join(__dirname, '..');
const engine = path.join(root, 'engine', 'quickjs-ng');
const ccacheDir = path.join(root, '.local', 'ccache');
const builtins = [
  'builtin-array-fromasync',
  'builtin-iterator-zip',
  'builtin-iterator-zip-keyed',
];
// One entry per VM build: its build directory, configure flags and blob suffix.
const vms = [
  {name: 'stack', dir: 'build-builtin-blobs', flags: [], suffix: '.h'},
  {
    name: 'register',
    dir: 'build-builtin-blobs-regvm',
    flags: ['-DCMAKE_C_COMPILER=clang', '-DCMAKE_C_FLAGS=-DQJS_REGVM=1'],
    suffix: '.regvm.h',
  },
];

function fail(message) {
  console.error(`\n[regenerate-builtin-blobs] ${message}\n`);
  process.exit(1);
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: root,
    encoding: 'utf8',
    maxBuffer: 64 * 1024 * 1024,
    ...options,
  });
  if (result.status !== 0) {
    const output = `${result.stdout || ''}${result.stderr || ''}`.trim();
    fail(
      `${command} ${args.join(' ')} failed` +
        (output ? `:\n\n${output}` : '.'),
    );
  }
  return result;
}

function writeAtomic(file, contents) {
  const temporary = `${file}.tmp-${process.pid}`;
  fs.writeFileSync(temporary, contents);
  fs.renameSync(temporary, file);
}

const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'rnqjs-builtins-'));

try {
  fs.mkdirSync(ccacheDir, {recursive: true});
  const buildEnvironment = {...process.env, CCACHE_DIR: ccacheDir};
  const changed = [];
  for (const vm of vms) {
    const buildDir = path.join(engine, vm.dir);
    run(
      'cmake',
      ['-S', engine, '-B', buildDir, '-DCMAKE_BUILD_TYPE=Release', ...vm.flags],
      {env: buildEnvironment},
    );
    run('cmake', ['--build', buildDir, '--target', 'qjsc', '--parallel'], {
      env: buildEnvironment,
    });

    const qjsc = path.join(buildDir, 'qjsc');
    const names = builtins.map(base => base + vm.suffix);
    for (const name of names) {
      run(
        qjsc,
        ['-C', '-ss', '-o', path.join(scratch, name), name.replace(vm.suffix, '.js')],
        {cwd: engine},
      );
    }
    // Whitespace is ignored, so a blob whose bytes are unchanged is kept as committed.
    const content = file => fs.readFileSync(file, 'utf8').replace(/\s+/g, '');
    for (const name of names) {
      if (content(path.join(engine, name)) !== content(path.join(scratch, name))) {
        writeAtomic(path.join(engine, name), fs.readFileSync(path.join(scratch, name)));
        changed.push(name);
      }
    }

    // The engine embeds the blobs, so qjs is rebuilt after they are written.
    run('cmake', ['--build', buildDir, '--target', 'qjs_exe', '--parallel'], {
      env: buildEnvironment,
    });
    run(path.join(buildDir, 'qjs'), [
      '-e',
      'const actual = [typeof Array.fromAsync, typeof Iterator.zip, ' +
        'typeof Iterator.zipKeyed, JSON.stringify([...Iterator.zip(' +
        '[[1, 2], [3, 4]])])].join("|"); ' +
        'if (actual !== "function|function|function|[[1,3],[2,4]]") ' +
        'throw new Error(actual);',
    ]);
  }
  console.log(
    changed.length === 0
      ? '[regenerate-builtin-blobs] built-in blobs are up to date'
      : `[regenerate-builtin-blobs] regenerated ${changed.join(', ')}: commit them in engine/quickjs-ng (develop), then the pointer`,
  );
} finally {
  fs.rmSync(scratch, {recursive: true, force: true});
}
