/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Rebuilds QuickJS's embedded bytecode with the compiler produced by the
 * complete patch stack, updates the final bytecode-version patch, and refreshes
 * engine/quickjs-rel.
 */

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const {spawnSync} = require('child_process');

const root = path.join(__dirname, '..');
const engine = path.join(root, 'engine', 'quickjs-ng');
const patchPath = path.join(
  root,
  'engine',
  'patches',
  '9999-bc-version-bump.patch',
);
const buildDir = path.join(engine, 'build-builtin-blobs');
const ccacheDir = path.join(root, '.local', 'ccache');
const files = [
  'quickjs.c',
  'builtin-array-fromasync.h',
  'builtin-iterator-zip.h',
  'builtin-iterator-zip-keyed.h',
];
const builtins = files.slice(1);

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

function copyInto(dir) {
  fs.mkdirSync(dir, {recursive: true});
  for (const name of files) {
    fs.copyFileSync(path.join(engine, name), path.join(dir, name));
  }
}

function diffFile(beforeDir, afterDir, name) {
  const result = spawnSync(
    'git',
    ['diff', '--no-index', '--', `before/${name}`, `after/${name}`],
    {
      cwd: path.dirname(beforeDir),
      encoding: 'utf8',
      maxBuffer: 64 * 1024 * 1024,
    },
  );
  if (result.status === 0) return '';
  if (result.status !== 1) {
    fail(`could not create the 9999 diff for ${name}:\n${result.stderr}`);
  }
  return result.stdout
    .replaceAll('a/before/', 'a/')
    .replaceAll('b/after/', 'b/');
}

run(process.execPath, ['scripts/apply-patches.js', '--check']);

const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'rnqjs-builtins-'));
const beforeDir = path.join(scratch, 'before');
const afterDir = path.join(scratch, 'after');

try {
  copyInto(beforeDir);
  copyInto(afterDir);

  run(
    'git',
    [
      'apply',
      '--reverse',
      '--whitespace=nowarn',
      '--unidiff-zero',
      patchPath,
    ],
    {cwd: beforeDir},
  );

  fs.mkdirSync(ccacheDir, {recursive: true});
  const buildEnvironment = {...process.env, CCACHE_DIR: ccacheDir};
  run(
    'cmake',
    ['-S', engine, '-B', buildDir, '-DCMAKE_BUILD_TYPE=Release'],
    {env: buildEnvironment},
  );
  run('cmake', [
    '--build',
    buildDir,
    '--target',
    'qjsc',
    '--parallel',
  ], {env: buildEnvironment});

  const qjsc = path.join(buildDir, 'qjsc');
  for (const name of builtins) {
    const source = name.replace(/\.h$/, '.js');
    run(
      qjsc,
      ['-C', '-ss', '-o', path.join(afterDir, name), source],
      {cwd: engine},
    );
  }

  const changed = builtins.filter(
    name =>
      !fs
        .readFileSync(path.join(engine, name))
        .equals(fs.readFileSync(path.join(afterDir, name))),
  );

  if (changed.length === 0) {
    console.log('[regenerate-builtin-blobs] built-in blobs are up to date');
  } else {
    const oldPatch = fs.readFileSync(patchPath, 'utf8');
    const marker = oldPatch.match(/^(?:diff --git |--- a\/)/m);
    if (!marker) fail('9999 has no patch body');

    const header = oldPatch.slice(0, marker.index);
    const body = files
      .map(name => diffFile(beforeDir, afterDir, name))
      .join('');
    if (!body) fail('regenerated 9999 would contain no diff');

    const candidate = path.join(scratch, 'candidate');
    copyInto(candidate);
    run(
      'git',
      [
        'apply',
        '--reverse',
        '--whitespace=nowarn',
        '--unidiff-zero',
        patchPath,
      ],
      {cwd: candidate},
    );
    const candidatePatch = path.join(scratch, '9999.patch');
    fs.writeFileSync(candidatePatch, header + body);
    run(
      'git',
      ['apply', '--whitespace=nowarn', '--unidiff-zero', candidatePatch],
      {cwd: candidate},
    );
    for (const name of files) {
      if (
        !fs
          .readFileSync(path.join(candidate, name))
          .equals(fs.readFileSync(path.join(afterDir, name)))
      ) {
        fail(`regenerated 9999 does not reproduce ${name}`);
      }
    }

    writeAtomic(patchPath, header + body);
    for (const name of builtins) {
      writeAtomic(
        path.join(engine, name),
        fs.readFileSync(path.join(afterDir, name)),
      );
    }
    console.log(
      `[regenerate-builtin-blobs] regenerated ${changed.join(', ')}`,
    );
  }

  run(process.execPath, ['scripts/sync-quickjs-rel.js'], {stdio: 'inherit'});
  run(
    'cmake',
    ['--build', buildDir, '--target', 'qjs_exe', '--parallel'],
    {env: buildEnvironment},
  );
  run(path.join(buildDir, 'qjs'), [
    '-e',
    'const actual = [typeof Array.fromAsync, typeof Iterator.zip, ' +
      'typeof Iterator.zipKeyed, JSON.stringify([...Iterator.zip(' +
      '[[1, 2], [3, 4]])])].join("|"); ' +
      'if (actual !== "function|function|function|[[1,3],[2,4]]") ' +
      'throw new Error(actual);',
  ]);
  run(process.execPath, ['scripts/apply-patches.js', '--check'], {
    stdio: 'inherit',
  });
  run(process.execPath, ['scripts/sync-quickjs-rel.js', '--check'], {
    stdio: 'inherit',
  });
} finally {
  fs.rmSync(scratch, {recursive: true, force: true});
}
