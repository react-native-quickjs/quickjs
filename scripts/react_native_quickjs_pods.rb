# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Podfile helper for apps running on QuickJS.
#
#   require_relative '../node_modules/@react-native-quickjs/quickjs/scripts/react_native_quickjs_pods.rb'
#
#   target 'App' do
#     use_quickjs!                                    # before use_react_native!
#     use_react_native!(:path => config[:reactNativePath])
#
#     post_install do |installer|
#       react_native_post_install(installer, config[:reactNativePath])
#       react_native_quickjs_post_install(installer)  # after
#     end
#   end

# react-native-worklets and react-native-reanimated declare
# `s.dependency 'React-hermes'` unconditionally, and use_quickjs! never declares
# that pod, so `pod install` cannot resolve it. Neither uses anything from it:
# their only Hermes include is <hermes/hermes.h>, which the compatibility shim
# provides.
#
# Dropped as each podspec is read, rather than by editing node_modules, so a
# reinstall does not undo it. React Native's own podspecs are not affected: they
# declare the same dependency behind `if use_hermes()`, which is already false.
#
# Only done when the shim is installed, so that without it a library asking for
# the real Hermes still fails loudly rather than silently losing it.
def react_native_quickjs_drop_react_hermes
  return if ENV["RNQJS_HERMES_COMPAT"] == "0"

  dependency = Pod::Specification.instance_method(:dependency)
  Pod::Specification.send(:define_method, :dependency) do |*args, &block|
    next if args.first.to_s == "React-hermes"

    dependency.bind(self).call(*args, &block)
  end
end

# Removes Hermes. Must run before use_react_native!, which reads all of this as
# the podspecs are evaluated.
#
# ENV['USE_HERMES'] is deliberately not set. React Native's own use_hermes() is
# `!use_third_party_jsc()` and never reads it, so setting it would change
# nothing here -- while any podspec that does read it directly, as Expo's does,
# reads USE_HERMES=0 as "use JavaScriptCore" rather than "use neither".
def use_quickjs!
  # Turns off every `if use_hermes()` dependency on hermes-engine at once.
  ENV['USE_THIRD_PARTY_JSC'] = '1'

  react_native_quickjs_drop_react_hermes

  # On the prebuilt path hermesvm.framework carries the JSI implementation, and
  # React-jsi.podspec drops its own jsi.cpp whenever Hermes is on. Removing
  # Hermes there leaves every runtime, ours included, unable to link. Consumers
  # pay for this in first-build and cold CI time.
  ENV['RCT_USE_PREBUILT_RNCORE'] = '0'

  # These declare Hermes pods in the Podfile directly, so they never consult
  # use_hermes(). use_react_native! reaches them through `hermes_enabled`, which
  # react_native_pods.rb:81 assigns true unconditionally -- replacing the
  # functions is the only way to stop them.
  Object.send(:define_method, :setup_hermes!) { |**_| }
  Object.send(:define_method, :depend_on_js_engine) { |_spec| }

  bridgeless = Object.instance_method(:setup_bridgeless!)
  Object.send(:define_method, :setup_bridgeless!) do |**kwargs|
    bridgeless.bind(self).call(**kwargs.merge(:use_hermes => false))
  end

  Pod::UI.puts(
    "[ReactNativeQuickJS] Hermes removed — JavaScript runs on QuickJS.".green
  )
end

# Compiles the release bundle to QuickJS bytecode, in a build phase placed
# right after React Native's own bundling phase. Debug builds load JavaScript
# from Metro and never produce a bundle, so the script exits early there.
BYTECODE_PHASE = '[ReactNativeQuickJS] Compile JavaScript to bytecode'

BYTECODE_SCRIPT = <<~SH
  set -e
  [ "$CONFIGURATION" = "Debug" ] && exit 0
  [ "$RNQJS_BYTECODE" = "0" ] && exit 0

  # The same files React Native's own bundle phase reads NODE_BINARY from.
  [ -f "$SRCROOT/.xcode.env" ] && . "$SRCROOT/.xcode.env"
  [ -f "$SRCROOT/.xcode.env.local" ] && . "$SRCROOT/.xcode.env.local"
  : "${NODE_BINARY:=node}"

  # Resolved through node, so a hoisted node_modules is found.
  COMPILER=$("$NODE_BINARY" --print \
    "require.resolve('@react-native-quickjs/quickjs/scripts/bytecode/compile.js', {paths: ['$SRCROOT']})")

  "$NODE_BINARY" "$COMPILER" \
    "$CONFIGURATION_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/main.jsbundle"
SH

def react_native_quickjs_add_bytecode_phase(installer)
  installer.aggregate_targets.map(&:user_project).uniq(&:path).compact.each do |project|
    project.native_targets.each do |target|
      next unless target.product_type == 'com.apple.product-type.application'
      next if target.build_phases.any? { |phase| phase.display_name == BYTECODE_PHASE }

      phase = target.new_shell_script_build_phase(BYTECODE_PHASE)
      phase.shell_script = BYTECODE_SCRIPT

      # Ordering matters: the bundle has to exist before it can be compiled.
      after = target.build_phases.index do |other|
        other.display_name.to_s.include?('Bundle React Native code and images')
      end
      next if after.nil?

      target.build_phases.delete(phase)
      target.build_phases.insert(after + 1, phase)
    end
    project.save
  end
end

def react_native_quickjs_post_install(installer)
  react_native_quickjs_add_bytecode_phase(installer)

  # Debug only, matching the gate React Native puts on its own inspector.
  # QuickJSInstance::debuggerEnabledByDefault() already refuses to attach in a
  # release build, so this keeps the compiled surface in agreement.
  react_native_quickjs_append(
    installer, "ReactNativeQuickJS", "GCC_PREPROCESSOR_DEFINITIONS",
    "RNQJS_ENABLE_CDP=1", :debug_only => true
  )

  # RCTCxxBridge.mm imports Hermes under `#if !defined(USE_HERMES)`, and
  # js_engine_flags() never defines it in the non-Hermes case.
  react_native_quickjs_append(
    installer, "React-Core", "GCC_PREPROCESSOR_DEFINITIONS", "USE_HERMES=0"
  )

  # RCTAppSetupUtils.h imports Hermes under `#if USE_THIRD_PARTY_JSC != 1`, and
  # React-RCTAppDelegate.podspec:21 loses the flag to a missing space -- clang
  # receives the two joined, as -DRCT_NEW_ARCH_ENABLED=1-DUSE_THIRD_PARTY_JSC=1.
  #
  # Every pod target, not just React-RCTAppDelegate: any pod including that
  # header resolves the same #if, and Expo's does, through RCTAppDelegateUmbrella
  # -- so a targeted define builds a plain app and fails an Expo one.
  react_native_quickjs_append_all(
    installer, "GCC_PREPROCESSOR_DEFINITIONS", "USE_THIRD_PARTY_JSC=1"
  )

  # With the Hermes compatibility shim installed, every pod must be able to
  # resolve <hermes/hermes.h> -- react-native-worklets decides which engine it
  # is built for with __has_include on exactly that path. The headers cannot be
  # published as public headers of this pod; see the HermesCompat subspec.
  if ENV["RNQJS_HERMES_COMPAT"] != "0"
    shim = File.expand_path("../modules/hermes-compat/include", __dir__)
    react_native_quickjs_append_all(installer, "HEADER_SEARCH_PATHS", "\"#{shim}\"")
    react_native_quickjs_append_all(
      installer, "GCC_PREPROCESSOR_DEFINITIONS", "HERMES_ENABLE_DEBUGGER=1"
    )
  end

  # createJSRuntimeFactory has an empty body under USE_THIRD_PARTY_JSC=1, which
  # -Werror,-Wreturn-type rejects. Reachable only in an app that does not
  # override it, and overriding it is how an app selects QuickJS at all.
  react_native_quickjs_append(
    installer, "React-RCTAppDelegate", "OTHER_CFLAGS", "-Wno-error=return-type"
  )

  # react-native-xcode.sh compares against the literal text `false`, so `0`,
  # `NO` and `FALSE` all read as "use Hermes" and a release build ships a
  # bytecode bundle QuickJS cannot execute.
  installer.aggregate_targets
    .map { |t| t.user_project }
    .uniq { |p| p.path }
    .push(installer.pods_project)
    .compact
    .each do |project|
      project.build_configurations.each do |config|
        config.build_settings["USE_HERMES"] = "false"
      end
      project.save()
    end

  Pod::UI.puts(
    "[ReactNativeQuickJS] USE_HERMES=false — release bundles are compiled to " \
    "QuickJS bytecode, not Hermes bytecode.".green
  )
end

# Raises when the pod is absent. Every caller repairs something that otherwise
# breaks the build, and they match React Native's pods by name -- exactly what a
# version bump renames -- so a silent no-op would resurface much later as the
# error it was meant to prevent.
# Applies a setting to every pod target. Used for a flag that selects which
# JavaScript engine header a React Native header imports: any pod that includes
# it needs the same answer, and which pods those are is not knowable here.
def react_native_quickjs_append_all(installer, setting, value)
  installer.target_installation_results.pod_target_installation_results.each_value do |result|
    result.native_target.build_configurations.each do |config|
      current = config.build_settings[setting] || "$(inherited)"
      current = current.join(" ") if current.is_a?(Array)
      config.build_settings[setting] = "#{current} #{value}"
    end
  end
end

def react_native_quickjs_append(installer, pod_name, setting, value, debug_only: false)
  result = installer.target_installation_results
    .pod_target_installation_results[pod_name]

  if result.nil?
    raise "[ReactNativeQuickJS] no pod named #{pod_name}, so #{value} was not " \
          "applied. React Native has probably renamed it; this needs updating " \
          "in scripts/react_native_quickjs_pods.rb."
  end

  result.native_target.build_configurations.each do |config|
    next if debug_only && config.type != :debug

    current = config.build_settings[setting] || "$(inherited)"
    current = current.join(" ") if current.is_a?(Array)
    config.build_settings[setting] = "#{current} #{value}"
  end
end
