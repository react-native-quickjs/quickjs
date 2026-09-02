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

const editFor = (label) => edits.find((edit) => edit.label === label);

function warnCouldNotEdit(label) {
  console.warn(
    `[@react-native-quickjs/quickjs] could not edit ${label}. ` +
      'Run `npx react-native-quickjs doctor` after prebuild.'
  );
}

/** Run one edit from edits.js over the contents of a string mod. */
function addQuickJS(label, source) {
  const edit = editFor(label);
  if (edit.isApplied(source)) return source;

  const edited = edit.addQuickJS(source);
  if (edited == null) {
    warnCouldNotEdit(label);
    return source;
  }
  return edited;
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
    cfg.modResults.contents = addQuickJS('android/app/build.gradle', cfg.modResults.contents);
    return cfg;
  });

const withAndroidFactory = (config) =>
  withMainApplication(config, (cfg) => {
    cfg.modResults.contents = addQuickJS('MainApplication.kt', cfg.modResults.contents);
    return cfg;
  });

const withIosFactory = (config) =>
  withAppDelegate(config, (cfg) => {
    cfg.modResults.contents = addQuickJS('AppDelegate.swift', cfg.modResults.contents);
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
        warnCouldNotEdit('ios/Podfile');
        return cfg;
      }
      fs.writeFileSync(file, addQuickJS('ios/Podfile', fs.readFileSync(file, 'utf8')));
      return cfg;
    },
  ]);

// Expo's ReactHost delegate hardcodes the engine:
//
//   override val jsRuntimeFactory: JSRuntimeFactory
//     get() = HermesInstance()
//
// ExpoReactHostFactory.getDefaultReactHost does take a jsRuntimeFactory
// parameter, and MainApplication.kt passes ours to it, but the function never
// hands it to the delegate it builds. On Expo the argument is accepted and
// ignored, and the app dies at launch looking for libhermestooling.so.
//
// So the parameter is carried the rest of the way here, which is what Expo
// itself would do. Only React Native types appear in the patch: the `expo`
// Gradle module does not depend on ours, so naming QuickJSInstance in that file
// does not compile. The concrete factory is still built by MainApplication.kt,
// in the app module, which does depend on ours.
//
// This edits node_modules, so a reinstall undoes it. Prebuild again after one.
const EXPO_REACT_HOST_FACTORY =
  'expo/android/src/main/java/expo/modules/ExpoReactHostFactory.kt';

const EXPO_ENGINE_PATCH = [
  [
    '    private val hostHandlers: List<ReactNativeHostHandler>\n  ) : ReactHostDelegate {',
    '    private val hostHandlers: List<ReactNativeHostHandler>,\n' +
      '    private val jsRuntimeFactoryOverride: JSRuntimeFactory? = null\n' +
      '  ) : ReactHostDelegate {',
  ],
  [
    '        hostHandlers = hostHandlers\n      )',
    '        hostHandlers = hostHandlers,\n' +
      '        jsRuntimeFactoryOverride = jsRuntimeFactory\n      )',
  ],
  [
    '    override val jsRuntimeFactory: JSRuntimeFactory\n      get() = HermesInstance()',
    '    override val jsRuntimeFactory: JSRuntimeFactory\n' +
      '      get() = jsRuntimeFactoryOverride ?: HermesInstance()',
  ],
];

const withExpoAndroidEngine = (config) =>
  withDangerousMod(config, [
    'android',
    (cfg) => {
      const file = path.join(cfg.modRequest.projectRoot, 'node_modules', EXPO_REACT_HOST_FACTORY);
      if (!fs.existsSync(file)) return cfg;

      const source = fs.readFileSync(file, 'utf8');
      if (source.includes('jsRuntimeFactoryOverride')) return cfg;

      // All three or none: a half-applied patch does not compile.
      if (!EXPO_ENGINE_PATCH.every(([find]) => source.includes(find))) {
        console.warn(
          '[@react-native-quickjs/quickjs] this version of Expo builds its ReactHost ' +
            'differently than the plugin expects, so the app would launch on Hermes. ' +
            'Please report this against @react-native-quickjs/quickjs.'
        );
        return cfg;
      }

      fs.writeFileSync(
        file,
        EXPO_ENGINE_PATCH.reduce((text, [find, replace]) => text.replace(find, replace), source)
      );
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
    withExpoAndroidEngine,
  ].reduce((c, mod) => mod(c), config);
};
