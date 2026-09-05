#!/usr/bin/env node
'use strict';

/*
 * Orchestrates a release of the root package (@react-native-quickjs/quickjs).
 *
 *   npm run release                # publish to latest
 *   npm run release:alpha          # publish to the alpha dist-tag
 *   node scripts/release.js --tag alpha --dry-run
 *
 * Gates, in order, so a bad package cannot reach the registry:
 *   1. engine integrity  check-submodule / apply-patches --check /
 *      sync-quickjs-rel --check / guard-selftest
 *   2. clean working tree and a package-lock.json that matches package.json
 *   3. a fresh `npm ci` (re-applies submodules and patches) followed by the
 *      full host test suite (configure + build + ctest)
 *   4. a `npm pack --dry-run` whose tarball is sanity-checked (engine present,
 *      submodule/node_modules absent)
 *   5. interactive: dist-tag, then version (default: the current package.json
 *      version if unpublished, else the next prerelease/patch). A version that
 *      is already on the registry is rejected.
 *   6. final confirm, then publish; on success the version bump is committed,
 *      tagged v<version> and pushed.
 *
 * `--dry-run` ends with `npm publish --dry-run` and skips commit/tag/push.
 * `--no-ci` and `--no-test` skip the two slow steps (for rehearsal only).
 */

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const readline = require('readline');

const ROOT = path.join(__dirname, '..');
const PKG_PATH = path.join(ROOT, 'package.json');
const LOCK_PATH = path.join(ROOT, 'package-lock.json');
const NAME = '@react-native-quickjs/quickjs';

const args = process.argv.slice(2);
const dryRun = args.includes('--dry-run');
const noCi = args.includes('--no-ci');
const noTest = args.includes('--no-test');
const force = args.includes('--force');
const flagTag = (() => {
  const i = args.indexOf('--tag');
  return i >= 0 && args[i + 1] ? args[i + 1] : null;
})();

const isTTY = Boolean(process.stdin.isTTY);
let pipedLines = [];

// One interface for the whole process. A fresh interface per prompt would hang
// a TTY run: the previous close() pauses stdin and the next never resolves.
const rl = isTTY
  ? readline.createInterface({ input: process.stdin, output: process.stdout })
  : null;

if (rl) {
  rl.on('SIGINT', () => {
    process.stdout.write('\nAborted.\n');
    process.exit(130);
  });
}

async function readPipedLines() {
  const lines = [];
  const collector = readline.createInterface({ input: process.stdin });
  for await (const line of collector) lines.push(line);
  return lines;
}

function ask(question) {
  if (rl) return new Promise((resolve) => rl.question(question, resolve));
  process.stdout.write(question + '\n');
  return Promise.resolve(pipedLines.length ? pipedLines.shift() : '');
}

function fail(message) {
  console.error(`\nrelease: ${message}\n`);
  process.exit(1);
}

function run(cmd, argv, opts = {}) {
  const result = spawnSync(cmd, argv, { cwd: ROOT, stdio: 'inherit', ...opts });
  if (result.error) fail(`${cmd} could not be started: ${result.error.message}`);
  if (result.status !== 0) process.exit(result.status === null ? 1 : result.status);
  return result;
}

function capture(cmd, argv, opts = {}) {
  const result = spawnSync(cmd, argv, { cwd: ROOT, encoding: 'utf8', ...opts });
  if (result.error) fail(`${cmd} could not be started: ${result.error.message}`);
  return result;
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function writeJson(file, data) {
  fs.writeFileSync(file, JSON.stringify(data, null, 2) + '\n');
}

const readPkg = () => readJson(PKG_PATH);
const currentVersion = () => readPkg().version;

/* --- semver helpers (mirrors scripts/release-module.js) ------------------- */

const BUMP_KEYWORDS = new Set([
  'major', 'minor', 'patch', 'premajor', 'preminor', 'prepatch', 'prerelease',
]);

function computeNext(current, keyword) {
  const m = /^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$/.exec(String(current));
  if (!m) throw new Error(`cannot bump non-semver version ${current}`);
  let major = Number(m[1]);
  let minor = Number(m[2]);
  let patch = Number(m[3]);
  let pre = m[4] || null;
  const nextPrerelease = () => {
    if (pre === null) {
      pre = '0';
    } else {
      const num = /^(.*?)(\d+)$/.exec(pre);
      pre = num ? num[1] + (Number(num[2]) + 1) : pre + '.0';
    }
  };
  switch (keyword) {
    case 'major': major += 1; minor = 0; patch = 0; pre = null; break;
    case 'minor': minor += 1; patch = 0; pre = null; break;
    case 'patch': patch += 1; pre = null; break;
    case 'premajor': major += 1; minor = 0; patch = 0; nextPrerelease(); break;
    case 'preminor': minor += 1; patch = 0; nextPrerelease(); break;
    case 'prepatch': patch += 1; nextPrerelease(); break;
    case 'prerelease': if (pre === null) patch += 1; nextPrerelease(); break;
    default: throw new Error(`unknown bump keyword ${keyword}`);
  }
  return `${major}.${minor}.${patch}${pre === null ? '' : '-' + pre}`;
}

/* --- engine and tree guards ---------------------------------------------- */

function runEngineGuard(script, label, extraArgs = []) {
  const r = capture(process.execPath, [path.join(ROOT, 'scripts', script), ...extraArgs]);
  if (r.status !== 0) {
    console.error(r.stdout || '');
    console.error(r.stderr || '');
    fail(`${label} FAILED -- refusing to release.`);
  }
  console.log(`ok    ${label}`);
}

function guardCleanTree() {
  const r = capture('git', ['status', '--porcelain']);
  const lines = (r.stdout || '').split('\n').map((s) => s.trim()).filter(Boolean);
  if (r.status !== 0) fail('not a git checkout; refusing to release.');
  if (lines.length) {
    if (force) {
      console.log('ok    working tree is dirty, but --force was passed');
      return;
    }
    console.error('\nWorking tree is not clean:\n');
    for (const line of lines) console.error('  ' + line);
    fail('commit or stash before releasing (or pass --force)');
  }
  console.log('ok    working tree is clean');
}

function guardLockMatches() {
  if (!fs.existsSync(LOCK_PATH)) {
    fail('package-lock.json is missing; run `npm install` first.');
  }
  const lockTop = readJson(LOCK_PATH).version;
  const lockRoot = readJson(LOCK_PATH).packages?.['']?.version;
  const pkg = currentVersion();
  if (lockTop !== pkg || lockRoot !== pkg) {
    fail(
      `package-lock.json is out of sync with package.json (lock ${lockTop} / ` +
        `${lockRoot}, package.json ${pkg}). Run: npm install --package-lock-only`
    );
  }
  console.log(`ok    package-lock.json matches package.json (${pkg})`);
}

function defaultNext(tag, current) {
  return computeNext(current, tag === 'alpha' ? 'prerelease' : 'patch');
}

/* Returns <0 / 0 / >0. Build metadata is ignored, as npm ignores it. */
function cmpVersions(a, b) {
  const pa = String(a).split('+')[0].split('-');
  const pb = String(b).split('+')[0].split('-');
  const na = pa[0].split('.').map(Number);
  const nb = pb[0].split('.').map(Number);
  for (let i = 0; i < 3; i++) {
    if (na[i] !== nb[i]) return na[i] - nb[i];
  }
  if (!pa[1] && !pb[1]) return 0;
  if (!pa[1]) return 1;  // a release outranks any prerelease
  if (!pb[1]) return -1;
  return pa[1] < pb[1] ? -1 : pa[1] > pb[1] ? 1 : 0;
}

/* --- published-versions check ------------------------------------------- */

function publishedVersions() {
  const r = capture('npm', ['view', NAME, 'versions', '--json'], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  if (r.status !== 0) return { ok: false, versions: [] };
  try {
    const list = JSON.parse(r.stdout);
    return { ok: true, versions: Array.isArray(list) ? list : [list] };
  } catch {
    return { ok: false, versions: [] };
  }
}

function bumpVersionInRoot(version) {
  const pkg = readJson(PKG_PATH);
  pkg.version = version;
  writeJson(PKG_PATH, pkg);

  const lock = readJson(LOCK_PATH);
  lock.version = version;
  if (lock.packages?.['']) lock.packages[''].version = version;
  writeJson(LOCK_PATH, lock);
}

async function main() {
  if (!isTTY) pipedLines = await readPipedLines();

  if (currentVersion().trim() === '' || readPkg().name !== NAME) {
    fail('expected to run from the root of @react-native-quickjs/quickjs');
  }

  console.log(`\n[1/6] Engine integrity for ${NAME}@${currentVersion()}\n`);
  runEngineGuard('check-submodule.js', 'engine submodule is at its recorded commit');
  runEngineGuard('apply-patches.js', 'engine patches are applied', ['--check']);
  runEngineGuard('sync-quickjs-rel.js', 'engine/quickjs-rel is in sync', ['--check']);
  runEngineGuard('guard-selftest.js', 'the engine guards can fail (self-test)');

  console.log(`\n[2/6] Tree and lockfile\n`);
  guardCleanTree();
  guardLockMatches();

  if (!noCi) {
    console.log('\n[3/6] Fresh install (npm ci) -- re-checks submodules and patches\n');
    run('npm', ['ci', '--no-audit', '--no-fund']);
    console.log('\nRe-checking the engine after the fresh install...\n');
    runEngineGuard('apply-patches.js', 'engine patches are applied after npm ci', ['--check']);
    runEngineGuard('sync-quickjs-rel.js', 'engine/quickjs-rel in sync after npm ci', ['--check']);
  } else {
    console.log('\n[3/6] Skipping npm ci (--no-ci)');
  }

  if (!noTest) {
    console.log('\n[4/6] Full host test suite\n');
    run('npm', ['test']);
  } else {
    console.log('\n[4/6] Skipping the test suite (--no-test)');
  }

  console.log('\n[5/6] Tarball sanity check\n');
  const pack = capture('npm', ['pack', '--dry-run', '--json', '--ignore-scripts']);
  if (pack.status !== 0) fail('npm pack --dry-run failed.');
  const packed = JSON.parse(pack.stdout);
  const files = (packed[0]?.files || []).map((f) => f.path);
  const names = new Set(files);
  const expectPresent = [
    'engine/quickjs-rel/quickjs.c',
    'engine/quickjs-rel/libunicode.c',
    'src/module/QuickJSModule.h',
    'scripts/postinstall.js',
    'bin/qjsc/darwin-arm64/qjsc',
  ];
  const missing = expectPresent.filter((f) => !names.has(f));
  const shouldBeAbsent = [
    'engine/quickjs-ng/quickjs.c',
    'engine/patches/0001-runtime-malloc-size-accessor.patch',
  ];
  const leaked = shouldBeAbsent.filter((f) => names.has(f));
  if (missing.length || leaked.length) {
    if (missing.length) console.error('  missing from tarball: ' + missing.join(', '));
    if (leaked.length) console.error('  must not ship: ' + leaked.join(', '));
    fail('tarball sanity check failed.');
  }
  const totalBytes = packed[0]?.unpackedSize || 0;
  console.log(
    `ok    tarball has ${files.length} files (${(totalBytes / 1024).toFixed(0)} KB unpacked); ` +
      'engine present, submodule and patches absent'
  );

  /* Dist-tag + version, interactive. */
  if (!dryRun && (noCi || noTest)) {
    fail('refusing a real publish with --no-ci/--no-test. Rehearse with --dry-run.');
  }

  const pub = publishedVersions();
  const publishedSet = new Set(pub.versions);
  if (!pub.ok) console.log('note: could not query the registry; duplicate versions are rejected there anyway.');

  const tag = flagTag || (await ask(`\n[6/6] dist-tag (latest/alpha) [default: latest]: `)).trim() || 'latest';

  const current = currentVersion();
  const currentUnpublished = !publishedSet.has(current);
  const suggested = currentUnpublished
    ? current
    : defaultNext(tag, current);

  let version;
  for (;;) {
    const def = currentUnpublished ? `${current} (unchanged)` : `${suggested}`;
    const raw = (await ask(`version [default: ${def}]: `)).trim();
    let candidate;
    if (raw === '') {
      candidate = currentUnpublished ? current : suggested;
    } else if (BUMP_KEYWORDS.has(raw)) {
      candidate = computeNext(current, raw);
    } else if (/^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/.test(raw)) {
      candidate = raw;
    } else {
      console.log('Enter a version like 1.0.0 / 1.0.0-alpha.1 or a bump keyword.');
      continue;
    }
    if (publishedSet.has(candidate)) {
      console.log(`${candidate} is already on the registry; pick a new version.`);
      continue;
    }
    if (candidate !== current && cmpVersions(candidate, current) < 0) {
      console.log(`${candidate} is older than the current ${current}; refusing.`);
      continue;
    }
    version = candidate;
    break;
  }

  const previewTag = tag === 'latest' ? 'latest' : tag;
  const after = dryRun
    ? ` Publish ${NAME}@${version} to "${previewTag}" as a DRY RUN? (y/N) `
    : ` Publish ${NAME}@${version} to "${previewTag}", then commit the bump, ` +
      `tag v${version} and push? (y/N) `;
  const answer = await ask('\n[6/6]' + after);
  if (!/^y(es)?$/i.test(answer.trim())) {
    console.log('Aborted. Nothing was changed.');
    process.exit(0);
  }

  if (version !== current) {
    console.log(`\nBumping ${current} -> ${version} (package.json + package-lock.json)...`);
    bumpVersionInRoot(version);
    console.log(`Bumped to ${version}.`);
  }

  const publishArgs = dryRun
    ? ['publish', '--dry-run', '--tag', previewTag]
    : ['publish', '--tag', previewTag];
  run('npm', publishArgs);

  if (dryRun) {
    console.log(`\nDRY RUN complete. ${NAME}@${version} was not published.`);
    console.log('No commit or tag was created.');
    process.exit(0);
  }

  if (version === current) {
    console.log(`\nNote: package.json was already at ${version}; no version commit needed.`);
  } else {
    console.log(`\nCommitting the version bump and tagging v${version}...`);
    run('git', ['add', 'package.json', 'package-lock.json']);
    let commit = capture('git', ['commit', '-m', `chore: release v${version}`], {
      stdio: ['ignore', 'inherit', 'inherit'],
    });
    if (commit.status !== 0) {
      console.log('First commit failed (identity/hook); retrying with the maintainer identity.');
      commit = capture(
        'git',
        ['-c', 'user.name=Ammar Ahmed', '-c', 'user.email=ammar@react-native-quickjs.org',
         'commit', '-m', `chore: release v${version}`],
        { stdio: ['ignore', 'inherit', 'inherit'] },
      );
    }
    if (commit.status !== 0) {
      fail('could not commit the version bump. Commit package.json and package-lock.json manually.');
    }
  }

  const existing = capture('git', ['tag', '-l', `v${version}`]).stdout.trim();
  if (!existing) {
    run('git', ['tag', '-a', `v${version}`, '-m', `${NAME} ${version}`]);
  }

  const branch = capture('git', ['rev-parse', '--abbrev-ref', 'HEAD']).stdout.trim();
  if (branch === 'HEAD') {
    console.error('Detached HEAD: publish succeeded but nothing was pushed. Push by hand.');
    process.exit(1);
  }
  const push = capture('git', ['push', 'origin', branch, `v${version}`]);
  if (push.status !== 0) {
    console.error(`\nPublished, but pushing ${branch} and v${version} failed:`);
    console.error(push.stderr || '');
    console.error(`Push them by hand:  git push origin ${branch} v${version}`);
    process.exit(1);
  }
  console.log(`\n${NAME}@${version} published as "${previewTag}", tagged v${version}, pushed.`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
}).finally(() => {
  if (rl) rl.close();
});
