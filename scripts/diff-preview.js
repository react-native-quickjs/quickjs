#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * A short diff for the engine checks to print when they fail.
 *
 * Naming the stale file is not enough on its own. The difference that has
 * actually happened here was two blank lines carrying trailing spaces, applied
 * on one machine and stripped on another by a git apply.whitespace setting --
 * invisible in a plain diff, and it cost a debugging branch to find. So this
 * calls whitespace-only differences out by name, and marks trailing whitespace
 * where it appears.
 */

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const MAX_LINES = 30;

function mark(line) {
  return line.replace(/[ \t]+$/, (ws) => `${'·'.repeat(ws.length)}`);
}

/**
 * @param {Buffer|string} expected what the file should contain
 * @param {Buffer|string} actual   what it contains
 * @param {string} name            the file's name, for the diff header
 * @returns {string}
 */
module.exports = function diffPreview(expected, actual, name) {
  const a = Buffer.from(expected).toString('utf8');
  const b = Buffer.from(actual).toString('utf8');

  const strip = (t) =>
    t
      .split('\n')
      .map((l) => l.replace(/[ \t]+$/, ''))
      .join('\n');
  const whitespaceOnly = a !== b && strip(a) === strip(b);

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'rnqjs-diff-'));
  const lines = [];
  try {
    const fa = path.join(dir, 'expected');
    const fb = path.join(dir, 'actual');
    fs.writeFileSync(fa, a);
    fs.writeFileSync(fb, b);

    const out = spawnSync(
      'git',
      ['diff', '--no-index', '--no-color', '-U1', '--', fa, fb],
      { encoding: 'utf8' }
    ).stdout;

    const body = out
      .split('\n')
      .filter(
        (l) =>
          !l.startsWith('diff --git') &&
          !l.startsWith('index ') &&
          !l.startsWith('--- ') &&
          !l.startsWith('+++ ')
      )
      .filter((l) => l.trim() !== '');

    lines.push(`  --- ${name} ---`);
    if (whitespaceOnly) {
      lines.push(
        '  The only difference is trailing whitespace, shown as · below.',
        '  A git apply.whitespace setting of `fix` strips it; the default',
        '  keeps it. scripts/apply-patches.js passes --whitespace=nowarn so',
        '  this cannot vary -- if you are seeing it, a patch was applied',
        '  without going through that script.'
      );
    }
    for (const line of body.slice(0, MAX_LINES)) {
      lines.push(`  ${mark(line)}`);
    }
    if (body.length > MAX_LINES) {
      lines.push(`  ... ${body.length - MAX_LINES} more line(s)`);
    }
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }

  return lines.join('\n');
};
