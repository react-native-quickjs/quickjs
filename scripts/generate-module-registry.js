#!/usr/bin/env node
/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Autolinking for QuickJS modules.
 *
 * Scans node_modules for packages declaring a `reactNativeQuickJSModule` field
 * and emits a C++ file that calls each module's install function explicitly.
 *
 *   node scripts/generate-module-registry.js [--out <file>] [--root <dir>]
 *
 * ## Why generate this rather than rely on static initializers
 *
 * `QJS_REGISTER_MODULE` puts a static initializer in the module's translation
 * unit, which works for a shared library. For a **static** library the linker
 * is entitled to drop any object file nothing references — and it does. The
 * module then compiles, links, and silently installs nothing, which is close to
 * the worst failure mode available: no error, no missing symbol, just a global
 * that never appears and a JavaScript polyfill quietly winning instead.
 *
 * Generated code that names each install function is a direct reference, so the
 * object file cannot be dropped. Both mechanisms are kept deliberately: the
 * static initializer is the convenient path during development, and this is the
 * one that survives a release build.
 *
 * Registration is idempotent and deduplicated by name (see registerModule), so
 * a module reached by both routes installs exactly once.
 */

const fs = require('node:fs');
const path = require('node:path');

// --- arguments -------------------------------------------------------------

const argv = process.argv.slice(2);
const opt = (name, dflt = null) => {
  const i = argv.indexOf(`--${name}`);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : dflt;
};

if (argv.includes('--help') || argv.includes('-h')) {
  console.log(`
generate-module-registry — autolink QuickJS modules

  node scripts/generate-module-registry.js [options]

Options
  --root <dir>   project root to scan from (default: cwd)
  --out <file>   output path (default: <root>/build/QuickJSGeneratedModules.cpp)
  --dir <dir>    an explicit module package directory (repeatable). When given,
                 the registry is exactly these modules -- authoritative, for an
                 app build that must only reference linked modules.
  --only <names> keep only these discovered modules (comma-separated names)
  --json         print the discovered modules as JSON and write nothing
  --quiet        suppress the summary`);
  process.exit(0);
}

const root = path.resolve(opt('root', process.cwd()));
const outPath = path.resolve(
  opt('out', path.join(root, 'build', 'QuickJSGeneratedModules.cpp'))
);
const only = opt('only', null) ? new Set(opt('only').split(',')) : null;
const asJson = argv.includes('--json');
const quiet = argv.includes('--quiet');

// --- discovery -------------------------------------------------------------

/**
 * Directories to scan. Monorepos hoist to the root `node_modules`, but a
 * workspace package may also carry its own, and this repository keeps its
 * first-party modules in `modules/` where they are not installed at all.
 */
function candidateDirs() {
  const dirs = [];
  const nodeModules = path.join(root, 'node_modules');
  if (fs.existsSync(nodeModules)) dirs.push(nodeModules);

  // First-party modules developed in-tree, which have no node_modules entry.
  const modules = path.join(root, 'modules');
  if (fs.existsSync(modules)) dirs.push(modules);

  return dirs;
}

/** Reads a package.json, returning null rather than throwing on bad input. */
function readPackage(dir) {
  try {
    const raw = fs.readFileSync(path.join(dir, 'package.json'), 'utf8');
    return JSON.parse(raw);
  } catch {
    return null;
  }
}

function moduleFromDir(dir) {
  const pkg = readPackage(dir);
  const spec = pkg && pkg.reactNativeQuickJSModule;
  if (!spec || !spec.install) return null;
  return {
    name: pkg.name || path.basename(dir),
    install: spec.install,
    header: spec.header || null,
    priority: typeof spec.priority === 'number' ? spec.priority : 0,
    dir,
  };
}

function discover() {
  const found = new Map(); // name -> { name, install, header, dir }

  for (const base of candidateDirs()) {
    let entries;
    try {
      entries = fs.readdirSync(base, { withFileTypes: true });
    } catch {
      continue;
    }

    for (const entry of entries) {
      if (!entry.isDirectory() && !entry.isSymbolicLink()) continue;

      // Scoped packages nest one level deeper: node_modules/@scope/name.
      const dirs = entry.name.startsWith('@')
        ? fs
            .readdirSync(path.join(base, entry.name), { withFileTypes: true })
            .filter((e) => e.isDirectory() || e.isSymbolicLink())
            .map((e) => path.join(base, entry.name, e.name))
        : [path.join(base, entry.name)];

      for (const dir of dirs) {
        const m = moduleFromDir(dir);
        if (m && !found.has(m.name)) found.set(m.name, m); // first wins
      }
    }
  }

  // Deterministic output: same inputs must produce a byte-identical file, or
  // every build looks like a change to the build system.
  return [...found.values()].sort(
    (a, b) => a.priority - b.priority || a.name.localeCompare(b.name)
  );
}

// Repeated --dir names an explicit module package directory. When any are
// given they are authoritative -- the registry becomes exactly those modules,
// in a deterministic order -- which is what an app build wants: node_modules
// contains packages that are present but not linked (transitive dependencies),
// and the registry must only reference symbols the app actually links, or the
// build fails on an undefined symbol.
const explicitDirs = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--dir' && argv[i + 1]) explicitDirs.push(path.resolve(argv[i + 1]));
}

let modules;
if (explicitDirs.length > 0) {
  const seen = new Map();
  for (const dir of explicitDirs) {
    const m = moduleFromDir(dir);
    if (m && !seen.has(m.name)) seen.set(m.name, m);
  }
  modules = [...seen.values()].sort(
    (a, b) => a.priority - b.priority || a.name.localeCompare(b.name)
  );
} else {
  modules = discover();
  if (only) {
    // An allow-list over discovered modules, for callers that scan node_modules
    // and know which of the results are actually linked.
    modules = modules.filter((m) => only.has(m.name));
  }
}

if (asJson) {
  console.log(JSON.stringify(modules, null, 2));
  process.exit(0);
}

// --- emit ------------------------------------------------------------------

const isValidIdentifier = (s) => /^[A-Za-z_][A-Za-z0-9_]*$/.test(s);

for (const m of modules) {
  if (!isValidIdentifier(m.install)) {
    console.error(
      `[generate-module-registry] ${m.name}: reactNativeQuickJSModule.install ` +
        `must be a C identifier, got ${JSON.stringify(m.install)}`
    );
    process.exit(1);
  }
}

const lines = [];
lines.push('// Generated by scripts/generate-module-registry.js — do not edit.');
lines.push('//');
lines.push('// Calls each autolinked module\'s install function by name. The explicit');
lines.push('// reference is the point: it stops the linker dropping a static library\'s');
lines.push('// object file, which would otherwise leave the module silently uninstalled.');
lines.push('');
lines.push('#include <jsi/jsi.h>');
lines.push('');
lines.push('#include "QuickJSModule.h"');
lines.push('');

if (modules.length === 0) {
  lines.push('// No autolinked modules were found.');
  lines.push('');
}

lines.push('extern "C" {');
for (const m of modules) {
  lines.push(`// ${m.name}`);
  lines.push(`void ${m.install}(facebook::jsi::Runtime &);`);
}
lines.push('}');
lines.push('');
lines.push('namespace qjs {');
lines.push('');
lines.push('void registerGeneratedModules() {');
if (modules.length === 0) {
  lines.push('  // Nothing to register.');
}
for (const m of modules) {
  lines.push(`  registerModule("${m.name}", ${m.install}, ${m.priority});`);
}
lines.push('}');
lines.push('');
lines.push('} // namespace qjs');
lines.push('');

const output = lines.join('\n');

fs.mkdirSync(path.dirname(outPath), { recursive: true });

// Only write when the content differs, so an unchanged registry does not
// retrigger a rebuild of everything downstream of it.
let existing = null;
try {
  existing = fs.readFileSync(outPath, 'utf8');
} catch {
  /* first run */
}

if (existing !== output) {
  fs.writeFileSync(outPath, output);
}

if (!quiet) {
  if (modules.length === 0) {
    console.log('[generate-module-registry] no QuickJS modules found');
  } else {
    console.log(
      `[generate-module-registry] ${modules.length} module(s) -> ${path.relative(root, outPath)}`
    );
    for (const m of modules) {
      console.log(`  ${m.name} -> ${m.install}()`);
    }
  }
}
