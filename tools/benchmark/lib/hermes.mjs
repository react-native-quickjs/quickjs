import { execFileSync, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, copyFileSync, statSync, symlinkSync, rmSync } from 'node:fs';
import { join, dirname, resolve, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';

const HERE = dirname(fileURLToPath(import.meta.url));
const BENCH = resolve(HERE, '..');
const BIN = join(BENCH, 'bin');
const MAC = join(BIN, 'mac');
const ANDROID = join(BIN, 'android', 'aarch64');
const ROOT = resolve(HERE, '..', '..', '..');

const HERMES_SRC = process.env.HERMES_SRC || '/Users/ammarahmed/Work/hermes/hermes';

function fail(msg) { const e = new Error(msg); e.code = 'HERMES_PROVISION'; throw e; }
function errMsg(e) { return e.code === 'HERMES_PROVISION' ? e.message : (e.stderr || e.message || String(e)); }

let _warnedLink = false;
async function copyIf(cachePath, srcCandidates) {
  for (const src of srcCandidates) {
    if (!existsSync(src) || statSync(src).size === 0) continue;
    try {
      copyFileSync(src, cachePath);
      return cachePath;
    } catch (err) {
      if (err.code === 'ENOSPC' || err.code === 'EMFILE' || err.code === 'EACCES') {
        try {
          rmSync(cachePath, { force: true });
          symlinkSync(src, cachePath);
          if (!_warnedLink) { console.error(`[hermes] disk full; linked ${cachePath} -> ${src}`); _warnedLink = true; }
          return cachePath;
        } catch (e) {
          console.error(`[hermes] copy and symlink both failed for ${cachePath}: ${e.message}`);
        }
      } else { throw err; }
    }
  }
  return null;
}

function haveBoth(local, remote) { return existsSync(local) && existsSync(remote); }

/* ---- host (macOS) hermes + hermesc -------------------------------------- */

function hostCache() { return { hermes: join(MAC, 'hermes'), hermesc: join(MAC, 'hermesc') }; }

function hostFromSource() { return { hermes: join(HERMES_SRC, 'build_release', 'bin', 'hermes'), hermesc: join(HERMES_SRC, 'build_release', 'bin', 'hermesc') }; }

function hostFromSourceAlt() { return { hermes: join(HERMES_SRC, 'build_android_p32', '..', 'build_release', 'bin', 'hermes'), hermesc: join(HERMES_SRC, 'build_host_hermesc', 'bin', 'hermesc') }; }

async function buildHost() {
  if (!existsSync(HERMES_SRC)) fail(`hermes source not found at ${HERMES_SRC}; set HERMES_SRC`);
  process.stderr.write('[hermes] building host (Release)...\n');
  const dir = join(HERMES_SRC, 'build_release');
  if (!existsSync(dir)) {
    execFileSync('cmake', ['-S', HERMES_SRC, '-B', dir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DHERMES_ENABLE_DEBUGGER=OFF', '-DHERMES_BUILD_APPLE_FRAMEWORK=OFF', '-DHERMES_BUILD_PODSPEC=OFF'], { stdio: 'inherit' });
  }
  execFileSync('cmake', ['--build', dir, '--target', 'hermes', 'hermesc'], { stdio: 'inherit' });
  return { hermes: join(dir, 'bin', 'hermes'), hermesc: join(dir, 'bin', 'hermesc') };
}

export async function ensureHostHermes() {
  const cache = hostCache();
  const src = hostFromSource();
  if (haveBoth(cache.hermes, cache.hermesc)) return { hermes: cache.hermes, hermesc: cache.hermesc, cached: true };

  mkdirSync(MAC, { recursive: true });
  if (haveBoth(src.hermes, src.hermesc) && (await copyIf(cache.hermes, [src.hermes])) && (await copyIf(cache.hermesc, [src.hermesc]))) {
    return { hermes: cache.hermes, hermesc: cache.hermesc, cached: false };
  }
  const built = buildHost();
  await copyIf(cache.hermes, [built.hermes]);
  await copyIf(cache.hermesc, [built.hermesc]);
  return { hermes: cache.hermes, hermesc: cache.hermesc, cached: false };
}

/* ---- android (aarch64) hermes VM ---------------------------------------- */

function androidCache() { return join(ANDROID, 'hermes'); }
function androidFromSource() { return join(HERMES_SRC, 'build_android_a64', 'bin', 'hermes'); }

async function buildAndroid() {
  if (!existsSync(HERMES_SRC)) fail(`hermes source not found at ${HERMES_SRC}; set HERMES_SRC`);
  process.stderr.write('[hermes] building android (aarch64)...\n');
  const dir = join(HERMES_SRC, 'build_android_a64');
  const toolchain = join(HERMES_SRC, 'android', 'ndk', 'build_android.sh');
  if (existsSync(toolchain)) {
    execFileSync('bash', [toolchain, 'build_release', 'android-arm64'], { stdio: 'inherit' });
  } else if (existsSync(dir)) {
    execFileSync('cmake', ['--build', dir, '--target', 'hermes'], { stdio: 'inherit' });
  } else {
    fail(`no android build of hermes and no build script at ${toolchain}`);
  }
  return join(dir, 'bin', 'hermes');
}

export async function ensureAndroidHermes() {
  const cache = androidCache();
  if (existsSync(cache)) return cache;
  mkdirSync(ANDROID, { recursive: true });
  const src = androidFromSource();
  if (existsSync(src) && (await copyIf(cache, [src]))) return cache;
  const built = await buildAndroid();
  await copyIf(cache, [built]);
  return cache;
}

export { HERMES_SRC, MAC, ANDROID };