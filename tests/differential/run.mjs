#!/usr/bin/env node
/**
 * Differential test runner: every corpus file is executed by this engine and by
 * node, and the two outputs are compared byte for byte.
 *
 * Byte-for-byte against node rather than a hand-written expectation, because an
 * expectation written by the same person who wrote the code encodes the same
 * misunderstanding. node is an independent implementation of the same
 * specification, so a diff is evidence rather than agreement with oneself.
 *
 * A corpus file is plain ES5, prints with `print()`, and must be deterministic:
 * no Math.random, no Date.now, and no iteration over host objects whose key set
 * differs between engines.
 *
 *   node tests/differential/run.mjs                        # all files
 *   node tests/differential/run.mjs for-in                 # one file, by prefix
 *   node tests/differential/run.mjs --qjs build/tests/qjs-run
 *   node tests/differential/run.mjs --via-bytecode         # through qjsc-ng
 *   node tests/differential/run.mjs --corpus pending       # a sibling directory
 *
 * `--via-bytecode` compiles each file with qjsc-ng and runs the container
 * instead of the source. That is the path React Native actually ships, and it
 * is not the same path: anything the compiler derives from source that the
 * serializer drops is present in one run and absent in the other, so a corpus
 * that only ever ran from source cannot see the difference.
 *
 * Exit status is non-zero on the first mismatch, and the difference is printed.
 * Any engine binary that runs a script and provides `print` works.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, writeFileSync, mkdtempSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..');
let CORPUS = join(HERE, 'corpus');

const args = process.argv.slice(2);
let qjs = null;
let qjsc = null;
let viaBytecode = false;
const filters = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--qjs') qjs = args[++i];
  else if (args[i] === '--qjsc') qjsc = args[++i];
  else if (args[i] === '--via-bytecode') viaBytecode = true;
  else if (args[i] === '--corpus') CORPUS = join(HERE, args[++i]);
  else filters.push(args[i]);
}

/*
 * Pick the NEWEST candidate, not the first one that exists.
 *
 * Taking the first hit in list order lets a stale release build shadow a
 * freshly built debug one, which reports spurious failures from source and a
 * bytecode version mismatch on every case through --via-bytecode. A harness
 * that chooses the binary it is validating has to say which one out loud, so
 * the chosen path is always printed.
 */
const newestOf = (cands) => {
  let best = null;
  for (const cand of cands) {
    const p = join(ROOT, cand);
    if (!existsSync(p)) continue;
    const t = statSync(p).mtimeMs;
    if (!best || t > best.t) best = { p, t };
  }
  return best?.p;
};

if (!qjs) {
  qjs = newestOf(['build-release/tests/qjs-run', 'build/tests/qjs-run']);
  if (qjs) console.error(`[diff] qjs (auto, newest): ${qjs}`);
}
if (!qjs || !existsSync(qjs)) {
  console.error('no engine binary found; build one with:\n' +
    '  cmake --build build -j8 --target qjs-run\n' +
    'or pass --qjs <path>');
  process.exit(2);
}

if (viaBytecode && !qjsc) {
  qjsc = newestOf(['build-release/qjsc-ng', 'build/qjsc-ng']);
  if (qjsc) console.error(`[diff] qjsc (auto, newest): ${qjsc}`);
}
if (viaBytecode && (!qjsc || !existsSync(qjsc))) {
  console.error('--via-bytecode needs qjsc-ng; build it with\n' +
    '  cmake --build build -j8 --target qjsc-ng\n' +
    'or pass --qjsc <path>');
  process.exit(2);
}

const tmp = mkdtempSync(join(tmpdir(), 'rnqjs-diff-'));
const files = readdirSync(CORPUS)
  .filter((f) => f.endsWith('.js'))
  .filter((f) => filters.length === 0 || filters.some((p) => f.startsWith(p)))
  .sort();

if (files.length === 0) {
  console.error('no corpus files matched');
  process.exit(2);
}

let failed = 0;
for (const f of files) {
  const src = join(CORPUS, f);
  // node has no `print`; give it one, in a wrapper so the corpus file itself
  // stays byte-identical between the two runs.
  const nodeWrapper = join(tmp, 'node-' + f);
  writeFileSync(nodeWrapper,
    `globalThis.print = (...a) => console.log(...a);\n` +
    `await import(${JSON.stringify(src)});\n`);

  let input = src;
  if (viaBytecode) {
    input = join(tmp, f.replace(/\.js$/, '.bc'));
    execFileSync(qjsc, [src, input], { stdio: 'ignore' });
  }

  let a, b;
  try {
    a = execFileSync(qjs, [input], { encoding: 'utf8', maxBuffer: 64 << 20 });
  } catch (e) {
    a = `<<qjs exited ${e.status}>>\n${e.stdout || ''}${e.stderr || ''}`;
  }
  try {
    b = execFileSync(process.execPath, ['--input-type=module', '-e',
      `globalThis.print = (...x) => console.log(...x); await import(${JSON.stringify(src)});`],
      { encoding: 'utf8', maxBuffer: 64 << 20 });
  } catch (e) {
    b = `<<node exited ${e.status}>>\n${e.stdout || ''}${e.stderr || ''}`;
  }

  if (a === b) {
    console.log(`ok    ${f}  (${a.split('\n').length - 1} lines identical)`);
  } else {
    failed++;
    console.log(`FAIL  ${f}`);
    const la = a.split('\n'), lb = b.split('\n');
    for (let i = 0; i < Math.max(la.length, lb.length); i++) {
      if (la[i] !== lb[i]) {
        console.log(`  line ${i + 1}`);
        console.log(`    qjs : ${JSON.stringify(la[i])}`);
        console.log(`    node: ${JSON.stringify(lb[i])}`);
      }
    }
  }
}
process.exit(failed ? 1 : 0);
