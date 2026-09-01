#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Turns ctest's JUnit output into the markdown that goes in the pull request
 * comment and the workflow artifact.
 */

'use strict';

const fs = require('fs');

const [, , input, output] = process.argv;

// A build that failed produced no results file. Say so and stop: the real
// error is further up the log, and a stack trace here only buries it.
if (!fs.existsSync(input)) {
  fs.writeFileSync(
    output,
    `### ${output.replace(/^report-|\.md$/g, '')}\n\n` +
      'The suites did not run -- the build failed before `ctest`.\n'
  );
  process.exit(1);
}

const xml = fs.readFileSync(input, 'utf8');

// Matched in two steps rather than one expression: a <testcase> may be
// self-closing or may wrap a <failure>, and one alternation over both swallows
// the next element's body along with this one's attributes.
const rows = [];
for (const m of xml.matchAll(/<testcase\b([^>]*?)(\/)?>/g)) {
  const attrs = m[1];
  const get = (k) => (attrs.match(new RegExp(`${k}="([^"]*)"`)) ?? [])[1] ?? '';
  let body = '';
  if (!m[2]) {
    const end = xml.indexOf('</testcase>', m.index);
    body = end === -1 ? '' : xml.slice(m.index + m[0].length, end);
  }
  rows.push({
    name: get('name'),
    time: Number(get('time') || 0),
    failed: /<failure\b/.test(body) || get('status') === 'fail',
  });
}

const failed = rows.filter((r) => r.failed);
const seconds = rows.reduce((t, r) => t + r.time, 0).toFixed(1);

const lines = [
  '### Host test suites',
  '',
  failed.length
    ? `**${failed.length} of ${rows.length} failed** in ${seconds}s`
    : `**All ${rows.length} passed** in ${seconds}s`,
  '',
  '| | suite | seconds |',
  '|---|---|---|',
  ...rows.map((r) => `| ${r.failed ? '✗' : '✓'} | \`${r.name}\` | ${r.time.toFixed(1)} |`),
];

fs.writeFileSync(process.argv[3] ?? '/dev/stdout', lines.join('\n') + '\n');
process.exit(failed.length ? 1 : 0);
