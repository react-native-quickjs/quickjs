#!/usr/bin/env node
'use strict';

/*
 * Orchestrates a module release.
 *
 *   npm run release:module -- intl
 *   npm run release:module -- @react-native-quickjs/intl
 *   npm run release:module -- --dry-run        (prompts for the module)
 *   npm run release:module
 *
 * Flow: pick the module (argument or prompt) -> run its build+test script ->
 * `npm publish --dry-run` to verify the tarball -> ask for the next version
 * (defaults to a patch bump; an empty answer accepts it) -> confirm -> bump
 * the workspace package.json -> publish. `--dry-run` makes the final step a
 * `npm publish --dry-run` so the whole flow can be rehearsed without
 * publishing anything.
 */

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const readline = require('readline');

const ROOT = path.join(__dirname, '..');
const MODULES_DIR = path.join(ROOT, 'modules');

const args = process.argv.slice(2);
const dryRun = args.includes('--dry-run');
const requested = args.find((a) => !a.startsWith('--'));

const isTTY = Boolean(process.stdin.isTTY);
let pipedLines = [];

// One interface for the whole process. Creating a fresh interface per prompt
// is what hangs a TTY run: the previous close() pauses stdin, so the next
// interface never receives input and its question never resolves.
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
  if (rl) {
    return new Promise((resolve) => rl.question(question, resolve));
  }
  process.stdout.write(question + '\n');
  return Promise.resolve(pipedLines.length ? pipedLines.shift() : '');
}

function fail(message) {
  console.error(`\nrelease:module: ${message}`);
  process.exit(1);
}

function run(command, argv, opts = {}) {
  const result = spawnSync(command, argv, {
    cwd: ROOT,
    stdio: 'inherit',
    ...opts,
  });
  if (result.error) fail(`${command} could not be started: ${result.error.message}`);
  if (result.status !== 0) {
    process.exit(result.status === null ? 1 : result.status);
  }
  return result;
}

function discoverModules() {
  const modules = [];
  for (const dir of fs.readdirSync(MODULES_DIR)) {
    const moduleDir = path.join(MODULES_DIR, dir);
    if (!fs.statSync(moduleDir).isDirectory()) continue;
    const pkgPath = path.join(moduleDir, 'package.json');
    if (!fs.existsSync(pkgPath)) continue;
    let pkg;
    try {
      pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
    } catch {
      continue;
    }
    if (!pkg.name || !pkg.name.startsWith('@react-native-quickjs/')) continue;
    modules.push({ dir, name: pkg.name, version: pkg.version, pkgPath });
  }
  return modules;
}

function findModule(modules, needle) {
  if (!needle) return null;
  const bare = needle.replace(/^@react-native-quickjs\//, '');
  return modules.find(
    (m) => m.name === needle || m.dir === needle || m.dir === bare,
  ) || null;
}

function defaultNextVersion(current) {
  return computeNext(current, 'patch');
}

const BUMP_KEYWORDS = new Set([
  'major', 'minor', 'patch',
  'premajor', 'preminor', 'prepatch', 'prerelease',
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
    case 'major':
      major += 1; minor = 0; patch = 0; pre = null; break;
    case 'minor':
      minor += 1; patch = 0; pre = null; break;
    case 'patch':
      patch += 1; pre = null; break;
    case 'premajor':
      major += 1; minor = 0; patch = 0; nextPrerelease(); break;
    case 'preminor':
      minor += 1; patch = 0; nextPrerelease(); break;
    case 'prepatch':
      patch += 1; nextPrerelease(); break;
    case 'prerelease':
      if (pre === null) { patch += 1; }
      nextPrerelease(); break;
    default:
      throw new Error(`unknown bump keyword ${keyword}`);
  }
  return `${major}.${minor}.${patch}${pre === null ? '' : '-' + pre}`;
}

function parseTarget(input, current) {
  const raw = String(input || '').trim();
  if (!raw) return defaultNextVersion(current);
  if (BUMP_KEYWORDS.has(raw)) return computeNext(current, raw);
  if (/^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/.test(raw)) return raw;
  return null;
}

function readVersion(pkgPath) {
  return JSON.parse(fs.readFileSync(pkgPath, 'utf8')).version;
}

function writeVersion(pkgPath, version) {
  const pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
  pkg.version = version;
  fs.writeFileSync(pkgPath, JSON.stringify(pkg, null, 2) + '\n');
}

async function pickModule(modules) {
  console.log('Modules:\n');
  modules.forEach((m, i) => {
    console.log(`  ${i + 1}) ${m.name}  (${m.version}, modules/${m.dir})`);
  });
  for (;;) {
    const answer = (await ask('\nModule to publish (number): ')).trim();
    const index = Number(answer) - 1;
    if (Number.isInteger(index) && index >= 0 && index < modules.length) {
      return modules[index];
    }
    console.log(`Enter a number between 1 and ${modules.length}.`);
  }
}

async function main() {
  if (!isTTY) pipedLines = await readPipedLines();

  const modules = discoverModules();
  if (modules.length === 0) fail('no publishable modules found under modules/');

  const chosen = findModule(modules, requested) || await pickModule(modules);
  if (!requested) console.log(`\nReleasing ${chosen.name}.`);

  console.log(`\n[1/4] Building and testing ${chosen.name}...\n`);
  run('npm', ['test', '--workspace', chosen.name]);

  console.log(`\n[2/4] Verifying the publish dry-run for ${chosen.name}...\n`);
  run('npm', ['publish', '--dry-run', '--workspace', chosen.name]);

  console.log(`\n[3/4] Next version for ${chosen.name} (currently ${chosen.version}).`);
  let target;
  for (;;) {
    const hint = `patch -> ${defaultNextVersion(chosen.version)}`;
    const answer = await ask(`Version [default: ${hint}]: `);
    target = parseTarget(answer, chosen.version);
    if (target) break;
    console.log('Enter a version like 1.0.1 or a bump keyword (major|minor|patch|premajor|preminor|prepatch|prerelease).');
  }

  const answer = await ask(`\n[4/4] Will publish ${target}. Continue? (y/N) `);
  if (!/^y(es)?$/i.test(answer.trim())) {
    console.log('Aborted. Nothing was bumped or published.');
    process.exit(0);
  }

  console.log(`\nBumping ${chosen.version} -> ${target}...`);
  writeVersion(chosen.pkgPath, target);
  const newVersion = readVersion(chosen.pkgPath);
  console.log(`Bumped to ${newVersion}.`);

  if (dryRun) {
    console.log('\n--dry-run: publishing to the registry is skipped.\n');
    run('npm', ['publish', '--dry-run', '--workspace', chosen.name]);
  } else {
    run('npm', ['publish', '--workspace', chosen.name]);
  }
  console.log(`\n${chosen.name}@${newVersion} published.`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
}).finally(() => {
  if (rl) rl.close();
});
