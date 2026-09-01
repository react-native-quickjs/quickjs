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

# Removes Hermes. Must run before use_react_native!, which reads all of this as
# the podspecs are evaluated.
#
# ENV['USE_HERMES'] is deliberately not set: on 0.85 it aborts pod install
# (error_if_try_to_use_jsc_from_core) and use_hermes() never reads it anyway.
def use_quickjs!
  # Turns off every `if use_hermes()` dependency on hermes-engine at once.
  ENV['USE_THIRD_PARTY_JSC'] = '1'

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

def react_native_quickjs_post_install(installer)
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

  # RCTAppSetupUtils.h needs USE_THIRD_PARTY_JSC, which is lost to a missing
  # space in React-RCTAppDelegate.podspec:21 -- clang receives the two flags
  # joined, as -DRCT_NEW_ARCH_ENABLED=1-DUSE_THIRD_PARTY_JSC=1.
  react_native_quickjs_append(
    installer, "React-RCTAppDelegate", "GCC_PREPROCESSOR_DEFINITIONS",
    "USE_THIRD_PARTY_JSC=1"
  )

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
    "[ReactNativeQuickJS] USE_HERMES=false — release bundles will be plain " \
    "JavaScript, not Hermes bytecode.".green
  )
end

# Raises when the pod is absent. Every caller repairs something that otherwise
# breaks the build, and they match React Native's pods by name -- exactly what a
# version bump renames -- so a silent no-op would resurface much later as the
# error it was meant to prevent.
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
