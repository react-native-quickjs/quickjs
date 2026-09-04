#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Build-time embedding of the Intl JavaScript layer.
 *
 * PURPOSE
 *   Turns js/intl.js into two generated headers:
 *
 *     IntlSource.h   a C string literal — always produced, always correct
 *     IntlBlob.h     QuickJS bytecode as a byte array — produced when a
 *                    qjsc-ng built from the *same* engine revision is available
 *
 *   The module tries the blob first and falls back to the source. That is not
 *   belt-and-braces for its own sake: QuickJS bytecode is loadable only by the
 *   engine build that produced it, and JS_ReadObject rejects a BC_VERSION
 *   mismatch with a clean SyntaxError rather than crashing
 *   (vendor/quickjs-ng/quickjs.c:44091), so the fallback converts "the engine
 *   was bumped and the blob is stale" from a broken app into a slower startup.
 *
 *   Which path a build actually takes is a *counted* fact, not an assumed one:
 *   build with -DRNQJS_INTL_COUNTERS=1 and read `intl.sourceLoads`. It must be
 *   zero in any build that quotes the precompiled startup number.
 *
 * THE SIZE GATE
 *   docs/intl-platform-backed.md sets a 60 KB source budget on the JavaScript
 *   layer, because the failure mode is gradual: one per-locale exception, then
 *   another, and it has become formatjs. This script fails the build above the
 *   budget. Without enforcement the budget is a sentence in a document.
 *
 * ASSEMBLY, AND WHY THE INPUT IS NOT ONE FILE
 *   js/intl.js may contain `//#include "name.js"` directives, which this script
 *   replaces with the named file from js/ before anything else happens. There is
 *   exactly one such include today — js/plural-data.js, the CLDR plural
 *   selectors — and it is a separate file because it is GENERATED (by
 *   scripts/gen-plural-data.js) while everything around it is hand-written. A
 *   generated 30 KB table pasted into the middle of a hand-written file makes
 *   every diff of that file unreadable.
 *
 *   The assembled source is written to <out>/intl.assembled.js and *that* is
 *   what qjsc-ng compiles, so a stack trace's line numbers refer to a file that
 *   exists and can be read. The per-file byte breakdown is printed, so the
 *   plural table is always a visible line item rather than something hiding
 *   inside a total.
 *
 * USAGE
 *   node scripts/embed-js.js --out cpp/generated [--qjsc /path/to/qjsc-ng]
 *                            [--max-source-bytes 180224] [--no-source]
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   It does not minify. The source is embedded verbatim so that a stack trace
 *   from inside the Intl layer points at a line you can read in js/intl.js, and
 *   because the blob — not the source — is what ships on the hot path anyway.
 */

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const os = require('os');

const HERE = path.resolve(__dirname, '..');
const INPUT = path.join(HERE, 'js', 'intl.js');

function parseArgs(argv) {
  const out = {
    outDir: path.join(HERE, 'cpp', 'generated'),
    qjsc: null,
    /*
     * The source budget. 184 KB of assembled JavaScript source.
     *
     * It was 60 KB through stage one (DateTimeFormat only) and 176 KB through
     * stage two. docs/intl-platform-backed.md Part 11 states the stage-two
     * breakdown and what the number bought. The rule the budget enforces has
     * NOT changed and is repeated in the failure message below: any *per-locale*
     * table other than js/plural-data.js and the alias tables is a budget
     * violation and belongs in a platform backend.
     *
     * RAISED FROM 176 KB TO 200 KB ON 2026-07-27, and the reason is written
     * down here rather than adjusted quietly. The performance work in
     * docs/intl-vs-node.md added the implicit-formatter memo and the
     * NumberFormat fast path. Both are algorithm, neither is data, and both are
     * documented at length in js/intl.js — the comments are the larger part of
     * what pushed the source over 176 KB and they are stripped before anything
     * ships.
     *
     * That is exactly why `maxBlob` below now exists, and why it is the gate
     * that should be believed. The *source* budget is a proxy; the blob is the
     * artefact that runs, and it is what the principle "never ship data the OS
     * already has" is actually about. A change that adds 8 KB of explanation
     * and 200 bytes of blob is not a budget violation, and before the blob gate
     * existed the build could not tell the two apart — it failed twice on
     * comments during the performance work, which is a gate training people to
     * write less down.
     *
     * The source gate is kept rather than deleted for one reason: by default
     * the assembled source is ALSO embedded in the binary, as the fallback for
     * a stale-BC_VERSION blob (see --no-source). So source bytes are real
     * binary bytes for a default build, just not runtime bytes.
     *
     * RAISED FROM 200 KB TO 212 KB ON 2026-07-27, and again the reason is
     * comments. The work in docs/intl-string-seam.md,
     * docs/intl-numberformat-double-path.md and docs/intl-lazy-segmentation.md
     * added four mechanisms — the canonicalization memo, the Segmenter's lazy
     * boundaries, the exact-double gate and the collator handle memo — whose
     * *code* is about 60 lines. The derivation of the exact-double magnitude
     * bound alone is 40 lines of comment, and it is the single most important
     * thing written in this module during that work, because an earlier
     * proposal asserted no such bound was needed and would have shipped wrong
     * digits. The blob — the bytes that actually ship — moved from 100,517 B
     * (CITED, docs/intl-vs-node.md) to 102,431 B (MEASURED) over the same
     * change: +1,914 B for four mechanisms, six counters and a bitmask on one
     * seam.
     *
     * If this gate fires again on comments, raise it again and write down why.
     * The gate that must not be raised without a data-versus-code argument is
     * `maxBlob` below.
     */
    max: 212 * 1024,
    /*
     * The blob budget. 100 KB of QuickJS bytecode — the bytes that ship.
     *
     * MEASURED 2026-07-27: 96,008 B at stage two (docs/intl-platform-backed.md
     * Part 11), 98,868 B after the implicit-formatter memo and the NumberFormat
     * fast path — +2,860 B, which is what those two mechanisms cost an app.
     * Comments and whitespace do not appear here, so this number moves only
     * when *code or data* is added, which is the thing the budget is for.
     *
     * The ceiling is set at measured + ~5%. It is deliberately tight: a budget
     * with 40% headroom does not fire until the thing it was protecting against
     * has already happened. It is only checked when a qjsc-ng is available; a
     * build with no compiler embeds source and has no blob to measure.
     */
    maxBlob: 104 * 1000,
    embedSource: true,
  };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === '--out') out.outDir = path.resolve(argv[++i]);
    else if (argv[i] === '--qjsc') out.qjsc = path.resolve(argv[++i]);
    else if (argv[i] === '--max-source-bytes') out.max = parseInt(argv[++i], 10);
    else if (argv[i] === '--max-blob-bytes') out.maxBlob = parseInt(argv[++i], 10);
    else if (argv[i] === '--no-source') out.embedSource = false;
    else {
      process.stderr.write(`embed-js: unknown option ${argv[i]}\n`);
      process.exit(2);
    }
  }
  return out;
}

/*
 * A C string literal, chunked one source line per literal.
 *
 * MSVC caps a single string literal at 16380 bytes and every compiler has a
 * limit on total literal length; concatenated adjacent literals sidestep both.
 * One literal per source line also means the generated header diffs the same
 * way the input does, which matters when reviewing a change to the Intl layer.
 */
function toCStringLiteral(text) {
  const lines = text.split('\n');
  const parts = lines.map((line) => {
    let esc = '';
    for (const ch of line) {
      const c = ch.codePointAt(0);
      if (ch === '\\') esc += '\\\\';
      else if (ch === '"') esc += '\\"';
      else if (ch === '?') esc += '\\?'; // defeats trigraphs, which are still on in C++14 mode on some toolchains
      else if (c < 0x20 || c === 0x7f) esc += '\\x' + c.toString(16).padStart(2, '0');
      else if (c > 0x7f) {
        // Escape non-ASCII so the header has no encoding dependency at all.
        for (const byte of Buffer.from(ch, 'utf8')) {
          esc += '\\x' + byte.toString(16).padStart(2, '0');
        }
      } else esc += ch;
    }
    return `    "${esc}\\n"`;
  });
  return parts.join('\n');
}

function toByteArray(buf) {
  const rows = [];
  for (let i = 0; i < buf.length; i += 16) {
    const row = [];
    for (let j = i; j < Math.min(i + 16, buf.length); j++) {
      row.push('0x' + buf[j].toString(16).padStart(2, '0'));
    }
    rows.push('    ' + row.join(', ') + ',');
  }
  return rows.join('\n');
}

const NSBC_HEADER_SIZE = 12;
const NSBC_MAGIC = 'NSBCNGS\0';

/**
 * Resolves `//#include "x.js"` directives against js/.
 *
 * Returns { text, parts } where `parts` is the per-file byte breakdown, which
 * is printed so that the generated plural table can never quietly become the
 * whole budget without anyone noticing.
 */
function assemble() {
  const root = fs.readFileSync(INPUT, 'utf8');
  const parts = [];
  let consumed = 0;
  const text = root.replace(/^[ \t]*\/\/#include\s+"([^"]+)"[ \t]*$/gm, (m, name) => {
    if (name.includes('/') || name.includes('..')) {
      throw new Error(`embed-js: include path must be a bare file name: ${name}`);
    }
    const file = path.join(HERE, 'js', name);
    const body = fs.readFileSync(file, 'utf8');
    parts.push([name, Buffer.byteLength(body, 'utf8')]);
    consumed += m.length;
    return body;
  });
  parts.unshift(['intl.js', Buffer.byteLength(root, 'utf8') - consumed]);
  return { text, parts };
}

function main() {
  const args = parseArgs(process.argv);
  const assembled = assemble();
  const source = assembled.text;
  const sourceBytes = Buffer.byteLength(source, 'utf8');

  if (sourceBytes > args.max) {
    process.stderr.write(
      `embed-js: the assembled Intl layer is ${sourceBytes} bytes, over the ` +
      `${args.max}-byte budget.\n` +
      assembled.parts.map(([n, b]) => `    ${n.padEnd(20)} ${b} B\n`).join('') +
      `  The budget and what it buys are documented in docs/intl-platform-backed.md.\n` +
      `  THE RULE IT ENFORCES: any per-locale table other than js/plural-data.js and\n` +
      `  the BCP-47 alias tables belongs in a platform backend, not in JavaScript.\n` +
      `  Apple has no plural API in any language, which is the whole reason\n` +
      `  js/plural-data.js is exempt; nothing else is.\n` +
      `  Raising the budget is a decision to be made and written down, not a build fix.\n`);
    process.exit(1);
  }

  fs.mkdirSync(args.outDir, { recursive: true });

  const header = (name, body) =>
    `/* GENERATED by modules/intl/scripts/embed-js.js — do not edit.\n` +
    ` * Source: modules/intl/js/intl.js (${sourceBytes} bytes)\n */\n#pragma once\n\n${body}\n`;

  // The assembled file is what qjsc-ng compiles, so it must exist on disk and
  // must be the exact bytes that were measured above.
  const assembledPath = path.join(args.outDir, 'intl.assembled.js');
  fs.writeFileSync(assembledPath, source);

  /*
   * --no-source drops the C string literal and leaves only the blob.
   *
   * It is off by default and it is a real trade rather than an optimisation:
   * the source is the fallback when JS_ReadObject rejects the blob's
   * BC_VERSION, so a --no-source build whose engine revision moves has no Intl
   * at all instead of a slower one. What it buys is the assembled source's
   * bytes out of every app binary. -DRNQJS_INTL_COUNTERS=1 and `intl.sourceLoads`
   * is how to find out whether a build was ever using the fallback before
   * turning it off.
   */
  fs.writeFileSync(
    path.join(args.outDir, 'IntlSource.h'),
    header('IntlSource',
      args.embedSource
        ? `static const char kIntlSource[] =\n${toCStringLiteral(source)};\n`
        : '/* --no-source: the blob is the only path. */\n' +
          'static const char kIntlSource[] = "";\n'));

  let haveBlob = 0;
  let blobBytes = 0;
  if (args.qjsc) {
    const tmp = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'rnqjs-intl-')), 'intl.bc');
    /*
     * --strip-source removes each function's embedded source text. It is safe
     * here for the same reason it is safe for a bundle: line numbers, column
     * numbers, filenames and pc2line are written under a different flag and are
     * untouched, so a stack trace from inside the Intl layer is unaffected.
     * What it costs is Function.prototype.toString on Intl internals, which no
     * caller can reach — every function this file exposes is reached through
     * the Intl namespace, and the spec does not define their source text.
     */
    execFileSync(args.qjsc, ['--strip-source', assembledPath, tmp], { stdio: 'inherit' });
    const raw = fs.readFileSync(tmp);
    if (raw.length < NSBC_HEADER_SIZE || raw.slice(0, 8).toString('binary') !== NSBC_MAGIC) {
      process.stderr.write('embed-js: qjsc-ng output is not an NSBCNGS container\n');
      process.exit(1);
    }
    // The module calls JS_ReadObject directly, so the container header goes.
    const payload = raw.slice(NSBC_HEADER_SIZE);
    blobBytes = payload.length;
    /*
     * The gate that matters. Source bytes include comments; blob bytes do not,
     * so this is the one that answers "did this change make the shipped
     * artefact bigger". It fires AFTER the blob is written, deliberately: the
     * generated header on disk is then available to diff against the previous
     * one when working out what grew.
     */
    if (blobBytes > args.maxBlob) {
      process.stderr.write(
        `embed-js: the compiled Intl blob is ${blobBytes} bytes, over the ` +
        `${args.maxBlob}-byte blob budget.\n` +
        `  These are the bytes that ship in an app. Unlike the source budget,\n` +
        `  comments and whitespace do not count here, so this number moved\n` +
        `  because CODE or DATA was added.\n` +
        `  THE RULE IT ENFORCES: any per-locale table other than js/plural-data.js\n` +
        `  and the BCP-47 alias tables belongs in a platform backend.\n` +
        `  Raising it is a decision to be written down, not a build fix.\n`);
      process.exit(1);
    }
    fs.writeFileSync(
      path.join(args.outDir, 'IntlBlob.h'),
      header('IntlBlob',
        `static const unsigned char kIntlBlob[] = {\n${toByteArray(payload)}\n};\n` +
        `static const unsigned long kIntlBlobSize = ${payload.length};\n`));
    haveBlob = 1;
  } else {
    /*
     * Always emit the header, so the include in IntlModule.cpp does not need to
     * be conditional on the file existing — only on the macro. A missing
     * generated header is a confusing build error; an empty one is not.
     */
    fs.writeFileSync(
      path.join(args.outDir, 'IntlBlob.h'),
      header('IntlBlob',
        '/* No qjsc-ng was available at build time; the module loads from source. */\n' +
        'static const unsigned char kIntlBlob[] = {0};\n' +
        'static const unsigned long kIntlBlobSize = 0;\n'));
  }

  fs.writeFileSync(
    path.join(args.outDir, 'IntlBuildConfig.h'),
    header('IntlBuildConfig', `#define RNQJS_INTL_HAVE_BLOB ${haveBlob}\n`));

  process.stdout.write(
    `embed-js: source ${sourceBytes} B (budget ${args.max} B, ` +
    `${((sourceBytes / args.max) * 100).toFixed(1)}% used), ` +
    `blob ${haveBlob ? blobBytes + ' B (budget ' + args.maxBlob + ' B, ' +
       ((blobBytes / args.maxBlob) * 100).toFixed(1) + '% used)' : 'not built'}` +
    `${args.embedSource ? '' : ', source NOT embedded'}\n` +
    assembled.parts.map(([n, b]) => `           ${n.padEnd(20)} ${b} B\n`).join(''));
}

main();
