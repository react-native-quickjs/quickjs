/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * The Expo config plugin. It runs the same edits as the CLI, but through
 * Expo's mods, so a prebuild regenerates them rather than losing them.
 *
 *   { "expo": { "plugins": ["@react-native-quickjs/quickjs"] } }
 */

'use strict';

const fs = require('fs');
const path = require('path');
const {
  withAppDelegate,
  withDangerousMod,
  withGradleProperties,
  withMainApplication,
  withAppBuildGradle,
} = require('@expo/config-plugins');

const { edits } = require('../setup/edits');

const byLabel = (label) => edits.find((e) => e.label === label);

function warn(label) {
  console.warn(
    `[@react-native-quickjs/quickjs] could not edit ${label}. ` +
      'Run `npx react-native-quickjs doctor` after prebuild.'
  );
}

/** Apply an edit to a string mod's contents. */
function edit(label, source) {
  const e = byLabel(label);
  if (e.done(source)) return source;
  const out = e.apply(source);
  if (out == null) {
    warn(label);
    return source;
  }
  return out;
}

// withGradleProperties hands over parsed items rather than text, so this one
// does not go through edits.js.
function withHermesOff(config) {
  return withGradleProperties(config, (cfg) => {
    const items = cfg.modResults.filter(
      (i) => !(i.type === 'property' && i.key === 'hermesEnabled')
    );
    items.push({ type: 'property', key: 'hermesEnabled', value: 'false' });
    cfg.modResults = items;
    return cfg;
  });
}

const withNoEngineDependency = (config) =>
  withAppBuildGradle(config, (cfg) => {
    cfg.modResults.contents = edit('android/app/build.gradle', cfg.modResults.contents);
    return cfg;
  });

const withAndroidFactory = (config) =>
  withMainApplication(config, (cfg) => {
    cfg.modResults.contents = edit('MainApplication.kt', cfg.modResults.contents);
    return cfg;
  });

const withIosFactory = (config) =>
  withAppDelegate(config, (cfg) => {
    cfg.modResults.contents = edit('AppDelegate.swift', cfg.modResults.contents);
    return cfg;
  });

// There is no withPodfile, so the Podfile is edited on disk after prebuild has
// written it.
const withPodfile = (config) =>
  withDangerousMod(config, [
    'ios',
    (cfg) => {
      const file = path.join(cfg.modRequest.platformProjectRoot, 'Podfile');
      if (!fs.existsSync(file)) {
        warn('ios/Podfile');
        return cfg;
      }
      fs.writeFileSync(file, edit('ios/Podfile', fs.readFileSync(file, 'utf8')));
      return cfg;
    },
  ]);

module.exports = function withQuickJS(config) {
  return [
    withHermesOff,
    withNoEngineDependency,
    withAndroidFactory,
    withIosFactory,
    withPodfile,
  ].reduce((c, mod) => mod(c), config);
};
