import { execFileSync, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, statSync } from 'node:fs';
import { join, dirname, basename, resolve } from 'node:path';
import { homedir } from 'node:os';

import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const BENCH = resolve(HERE, '..');
const ROOT = resolve(HERE, '..', '..', '..');
const ANDROID_BIN = join(BENCH, 'bin', 'android', 'aarch64');
const ELF_PATHS = [
  join(homedir(), 'Library/Android/sdk/platform-tools/adb'),
  join(homedir(), 'Android/Sdk/platform-tools/adb'),
  '/usr/local/bin/adb',
  '/opt/homebrew/bin/adb',
];
const NDK_HOME = process.env.ANDROID_NDK_HOME || join(homedir(), 'Library/Android/sdk/ndk/27.1.12297006');
const REMOTE_DIR = '/data/local/tmp/rnqjs';
const API = '26';

function adb() {
  for (const p of ELF_PATHS) if (existsSync(p)) return p;
  throw new Error('adb not found. Set ANDROID_HOME or install platform-tools.');
}

export function checkAdb() {
  const a = adb();
  const out = execFileSync(a, ['devices'], { encoding: 'utf8' });
  const lines = out.split('\n').filter(l => /\tdevice$/.test(l));
  if (lines.length === 0) throw new Error('no Android device connected (adb devices)');
  if (lines.length > 1) throw new Error(`${lines.length} devices connected; bench needs exactly one`);
  return lines[0].split('\t')[0];
}

function clang() {
  const prebuiltDir = join(NDK_HOME, 'toolchains/llvm/prebuilt');
  if (!existsSync(prebuiltDir)) throw new Error(`NDK not found at ${NDK_HOME}. Set ANDROID_NDK_HOME.`);
  const host = 'darwin-x86_64';
  return join(prebuiltDir, host, 'bin', `aarch64-linux-android${API}-clang`);
}

export function crossBuild(engineDir) {
  const c = clang();
  const outDir = ANDROID_BIN;
  mkdirSync(outDir, { recursive: true });
  const qjs = join(outDir, 'qjs-bench');
  const calls = join(outDir, 'native-call-bench');
  const src = () => [engineDir + '/quickjs.c', engineDir + '/libregexp.c', engineDir + '/libunicode.c', engineDir + '/dtoa.c'];
  run(c, ['-O2', '-o', qjs, join(BENCH, 'qjs-bench.c'), ...src(), `-I${engineDir}`, '-lm', '-fPIE', '-pie']);
  run(c, ['-O2', '-o', calls, join(BENCH, 'calls.c'), ...src(), `-I${engineDir}`, '-lm', '-fPIE', '-pie']);
  return { qjs, calls };
}

function run(cmd, args) {
  const r = spawnSync(cmd, args, { encoding: 'utf8' });
  if (r.status !== 0) throw new Error((r.stderr || '').slice(0, 800));
}

function sh(cmd, args) {
  const a = adb();
  const r = spawnSync(a, args, { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
  return r;
}

function remoteName(localPath) {
  return basename(localPath);
}

/** Push a local qjs-bench-flavoured run to the device, run it, and pull output.
 *  argv is the full argument vector [flags..., file]. Returns {status, stdout, stderr}. */
export function deviceRun(localBin, argv, remoteDir = REMOTE_DIR) {
  checkAdb();
  const a = adb();
  sh(a, ['shell', `mkdir -p ${remoteDir}`]);
  const remoteBin = `${remoteDir}/qjs-bench`;
  sh(a, ['push', localBin, remoteBin]);
  sh(a, ['shell', `chmod 755 ${remoteBin}`]);

  const fileArg = argv[argv.length - 1];
  const flags = argv.slice(0, -1);
  const remoteFile = `${remoteDir}/${remoteName(fileArg)}`;
  sh(a, ['push', fileArg, remoteFile]);

  const r = sh(a, ['shell', `${remoteBin} ${flags.join(' ')} ${remoteFile}`]);
  return r;
}

/** Push and run the native-call-bench driver on the device. */
export function deviceCallRun(localBin, args) {
  checkAdb();
  const a = adb();
  const remoteBin = `${REMOTE_DIR}/native-call-bench`;
  sh(a, ['push', localBin, remoteBin]);
  sh(a, ['shell', `chmod 755 ${remoteBin}`]);
  const r = sh(a, ['shell', `${remoteBin} ${args.join(' ')}`]);
  return r;
}

/** Push and run the hermes (android VM) binary on the device. argv is the full
 *  argument vector [flags..., file]; the file is pushed too. */
export function deviceHermesRun(localBin, argv) {
  checkAdb();
  const a = adb();
  const remoteBin = `${REMOTE_DIR}/hermes`;
  sh(a, ['shell', `mkdir -p ${REMOTE_DIR}`]);
  sh(a, ['push', localBin, remoteBin]);
  sh(a, ['shell', `chmod 755 ${remoteBin}`]);
  const fileArg = argv[argv.length - 1];
  const flags = argv.slice(0, -1);
  const remoteFile = `${REMOTE_DIR}/${remoteName(fileArg)}`;
  sh(a, ['push', fileArg, remoteFile]);
  return sh(a, ['shell', `${remoteBin} ${flags.join(' ')} ${remoteFile}`]);
}

export { adb };