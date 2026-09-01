/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The build-config edits that move an app between Hermes and QuickJS, written
 * once. `install` applies them, `revert` undoes them, `doctor` reports them,
 * and the Expo plugin runs the same functions inside Expo's own mods.
 *
 * Each edit is:
 *
 *   label         what to call the file in output
 *   findFile      absolute path in a project, or null if the project has none
 *   isApplied     is this file already configured for QuickJS?
 *   addQuickJS    returns the edited source, or null (see below)
 *   removeQuickJS the inverse
 *   manualSteps   what to tell someone when the functions return null
 *
 * Returning null means "this file does not look the way I expect". That is not
 * an error to throw -- the file has been customised, and the caller prints
 * manualSteps instead of guessing.
 */

'use strict';

const fs = require('fs');
const path = require('path');

const PACKAGE = '@react-native-quickjs/quickjs';
const KOTLIN_IMPORT = 'import com.reactnativequickjs.quickjs.QuickJSInstance';
const SWIFT_IMPORT = 'import ReactNativeQuickJS';
const PODFILE_REQUIRE = `require_relative '../node_modules/${PACKAGE}/scripts/react_native_quickjs_pods.rb'`;

const SKIP_DIRECTORIES = new Set(['build', 'node_modules', 'Pods']);

/** The first file called `fileName` anywhere under `directory`. */
function findFileNamed(directory, fileName, depth = 7) {
  if (depth < 0 || !fs.existsSync(directory)) return null;

  const subdirectories = [];
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    if (SKIP_DIRECTORIES.has(entry.name)) continue;
    const fullPath = path.join(directory, entry.name);
    if (entry.isDirectory()) subdirectories.push(fullPath);
    else if (entry.name === fileName) return fullPath;
  }

  for (const subdirectory of subdirectories) {
    const found = findFileNamed(subdirectory, fileName, depth - 1);
    if (found) return found;
  }
  return null;
}

/** Index of the `)` that closes the call whose `(` is at `openParen`. */
function closingParenIndex(source, openParen) {
  let depth = 0;
  for (let i = openParen; i < source.length; i++) {
    if (source[i] === '(') depth++;
    else if (source[i] === ')' && --depth === 0) return i;
  }
  return -1;
}

/** The source with `importLine` added after the last import, or null. */
function withImportAdded(source, importLine) {
  if (source.includes(importLine)) return source;

  const imports = [...source.matchAll(/^import .*$/gm)];
  if (imports.length === 0) return null;

  const lastImport = imports[imports.length - 1];
  const insertAt = lastImport.index + lastImport[0].length;
  return source.slice(0, insertAt) + '\n' + importLine + source.slice(insertAt);
}

/**
 * The source without `line`, and without the blank line that followed it.
 * A plain string match, so there is no regular expression to escape.
 */
function withLineRemoved(source, line) {
  return source.includes(line + '\n\n')
    ? source.replace(line + '\n\n', '')
    : source.replace(line + '\n', '');
}

/** The indentation of the first line at or after `index`. */
function indentAt(source, index, fallback) {
  const match = /^([ \t]+)\S/.exec(source.slice(index));
  return match ? match[1] : fallback;
}

const edits = [
  {
    label: 'android/gradle.properties',
    findFile: (project) => path.join(project, 'android', 'gradle.properties'),
    isApplied: (source) => /^[ \t]*hermesEnabled[ \t]*=[ \t]*false[ \t]*$/m.test(source),

    addQuickJS: (source) =>
      /^[ \t]*hermesEnabled[ \t]*=/m.test(source)
        ? source.replace(/^([ \t]*hermesEnabled[ \t]*=[ \t]*).*$/m, '$1false')
        : `${source.replace(/\s*$/, '')}\n\nhermesEnabled=false\n`,

    removeQuickJS: (source) =>
      source.replace(/^([ \t]*hermesEnabled[ \t]*=[ \t]*).*$/m, '$1true'),

    manualSteps: ['Set hermesEnabled=false in android/gradle.properties'],
  },

  {
    // The template picks Hermes or JavaScriptCore here, and hermesEnabled=false
    // is what sends it down the JavaScriptCore branch. An app on QuickJS wants
    // neither, so the whole block goes.
    label: 'android/app/build.gradle',
    findFile: (project) => path.join(project, 'android', 'app', 'build.gradle'),
    isApplied: (source) => !/hermes-android|jscFlavor/.test(source),

    addQuickJS(source) {
      const withoutEngines = source
        .replace(
          /[ \t]*if \([ \t]*hermesEnabled\.toBoolean\(\)[ \t]*\) \{[\s\S]*?\n[ \t]*\}[ \t]*\n/,
          ''
        )
        .replace(/[ \t]*def jscFlavor[ \t]*=.*\n/, '');
      return withoutEngines === source ? null : withoutEngines;
    },

    removeQuickJS(source) {
      const reactAndroid = /([ \t]*)implementation\("com\.facebook\.react:react-android"\)\n/;
      if (!reactAndroid.test(source)) return null;

      return source.replace(
        reactAndroid,
        (line, indent) =>
          `${line}\n${indent}if (hermesEnabled.toBoolean()) {\n` +
          `${indent}    implementation("com.facebook.react:hermes-android")\n` +
          `${indent}} else {\n` +
          `${indent}    implementation jscFlavor\n` +
          `${indent}}\n`
      );
    },

    manualSteps: [
      'In android/app/build.gradle, delete the dependencies block that picks',
      'between com.facebook.react:hermes-android and jscFlavor.',
    ],
  },

  {
    label: 'MainApplication.kt',
    findFile: (project) =>
      findFileNamed(path.join(project, 'android', 'app', 'src'), 'MainApplication.kt'),
    isApplied: (source) => source.includes('QuickJSInstance('),

    addQuickJS(source) {
      const withImport = withImportAdded(source, KOTLIN_IMPORT);
      if (!withImport) return null;

      const call = /getDefaultReactHost\(\n/.exec(withImport);
      if (!call) return null;

      // Added as the first argument rather than the last. Kotlin named
      // arguments may be given in any order, and the first one's indentation
      // is the only one readable without matching parens through the nested
      // PackageList lambda.
      const firstArgument = call.index + call[0].length;
      const indent = indentAt(withImport, firstArgument, '      ');

      return (
        withImport.slice(0, firstArgument) +
        `${indent}jsRuntimeFactory = QuickJSInstance(),\n` +
        withImport.slice(firstArgument)
      );
    },

    removeQuickJS: (source) =>
      withLineRemoved(source, KOTLIN_IMPORT).replace(
        /^[ \t]*jsRuntimeFactory = QuickJSInstance\(\),?[ \t]*\n/m,
        ''
      ),

    manualSteps: [
      'In MainApplication.kt add:',
      `  ${KOTLIN_IMPORT}`,
      'and pass jsRuntimeFactory = QuickJSInstance() to getDefaultReactHost().',
    ],
  },

  {
    label: 'ios/Podfile',
    findFile: (project) => path.join(project, 'ios', 'Podfile'),
    isApplied: (source) => source.includes('use_quickjs!'),

    addQuickJS(source) {
      // Every anchor is checked before anything is written, so a Podfile this
      // does not recognise is left alone rather than half configured.
      const hasPlatform = /^platform :ios/m.test(source);
      const useReactNative = /^([ \t]*)use_react_native!\(/m.exec(source);
      const postInstall = source.indexOf('react_native_post_install(');
      if (!hasPlatform || !useReactNative || postInstall === -1) return null;

      const withRequire = source.includes(PODFILE_REQUIRE)
        ? source
        : source.replace(/^platform :ios/m, `${PODFILE_REQUIRE}\n\nplatform :ios`);

      const withUseQuickJS = withRequire.replace(
        /^([ \t]*)use_react_native!\(/m,
        '$1use_quickjs!\n\n$1use_react_native!('
      );

      // The hook must follow React Native's own, which writes the USE_HERMES
      // build setting this overwrites.
      const call = withUseQuickJS.indexOf('react_native_post_install(');
      const closing = closingParenIndex(withUseQuickJS, withUseQuickJS.indexOf('(', call));
      if (closing === -1) return null;

      const endOfLine = withUseQuickJS.indexOf('\n', closing);
      const indent = /\n([ \t]*)$/.exec(withUseQuickJS.slice(0, call));

      return (
        withUseQuickJS.slice(0, endOfLine + 1) +
        `\n${indent ? indent[1] : '    '}react_native_quickjs_post_install(installer)\n` +
        withUseQuickJS.slice(endOfLine + 1)
      );
    },

    removeQuickJS: (source) =>
      withLineRemoved(source, PODFILE_REQUIRE)
        .replace(/^[ \t]*use_quickjs!\n\n?/m, '')
        .replace(/\n?[ \t]*react_native_quickjs_post_install\(installer\)\n/, '\n'),

    manualSteps: [
      'In ios/Podfile add, before use_react_native!:',
      `  ${PODFILE_REQUIRE}`,
      '  use_quickjs!',
      'and react_native_quickjs_post_install(installer) after react_native_post_install.',
    ],
  },

  {
    label: 'AppDelegate.swift',
    findFile: (project) => findFileNamed(path.join(project, 'ios'), 'AppDelegate.swift'),
    isApplied: (source) => source.includes('jsrt_create_quickjs_factory'),

    addQuickJS(source) {
      const withImport = withImportAdded(source, SWIFT_IMPORT);
      if (!withImport) return null;

      const delegateClass =
        /class\s+\w+\s*:\s*RCTDefaultReactNativeFactoryDelegate\s*\{/.exec(withImport);
      if (!delegateClass) return null;

      const classBody = delegateClass.index + delegateClass[0].length;
      return (
        withImport.slice(0, classBody) +
        '\n  override func createJSRuntimeFactory() -> JSRuntimeFactoryRef {\n' +
        '    jsrt_create_quickjs_factory()\n' +
        '  }\n' +
        withImport.slice(classBody)
      );
    },

    removeQuickJS: (source) =>
      withLineRemoved(source, SWIFT_IMPORT).replace(
        /\n[ \t]*override func createJSRuntimeFactory\(\)[^\n]*\{\n[ \t]*jsrt_create_quickjs_factory\(\)\n[ \t]*\}\n/,
        '\n'
      ),

    manualSteps: [
      'In AppDelegate.swift add:',
      `  ${SWIFT_IMPORT}`,
      'and override createJSRuntimeFactory() to return jsrt_create_quickjs_factory().',
    ],
  },
];

module.exports = { edits };
