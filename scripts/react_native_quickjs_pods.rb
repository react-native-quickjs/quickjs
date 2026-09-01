# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Podfile helper for apps running on QuickJS.
#
#   require_relative '../node_modules/@react-native-quickjs/quickjs/scripts/react_native_quickjs_pods.rb'
#
#   post_install do |installer|
#     react_native_post_install(installer, config[:reactNativePath])
#     react_native_quickjs_post_install(installer)   # <- must come AFTER
#   end
#
# ---------------------------------------------------------------------------
# WHY THIS IS NEEDED, and why `ENV['USE_HERMES'] = '0'` is NOT the answer
# ---------------------------------------------------------------------------
#
# The obvious way to say "this app does not use Hermes" is USE_HERMES=0 in the
# Podfile. On React Native 0.85 that is actively wrong, in two independent ways.
# Both were read out of the installed React Native rather than assumed:
#
# 1. IT ABORTS `pod install`. react_native_pods.rb:492,
#    `error_if_try_to_use_jsc_from_core`, is called from `use_react_native!`
#    (react_native_pods.rb:75) and does:
#
#        explicitly_not_use_hermes = ENV['USE_HERMES'] == '0'
#        not_use_3rd_party_jsc     = ENV['USE_THIRD_PARTY_JSC'] != '1'
#        if explicitly_not_use_hermes && not_use_3rd_party_jsc
#          puts "...Please remove the USE_HERMES=0..."
#          exit()
#        end
#
#    Setting it, on its own, ends the install.
#
# 2. IT WOULD NOT HELP ANYWAY. As of 0.85, cocoapods/jsengine.rb:29 defines
#
#        def use_hermes
#          return !use_third_party_jsc()      # USE_THIRD_PARTY_JSC == '1'
#        end
#
#    which does not consult ENV['USE_HERMES'] at all. So the build setting
#    react_native_post_install writes (react_native_pods.rb:531,
#    `set_build_setting(installer, build_setting: "USE_HERMES", value: use_hermes())`)
#    is `true` regardless of what the Podfile set.
#
# THE CONSEQUENCE, and it is a release-only app-killer:
#
# `scripts/react-native-xcode.sh` reads that build setting, and at line 166 does
#
#        if [[ $USE_HERMES == false ]]; then   copy the plain .jsbundle
#        else                                  run hermesc, emit BYTECODE
#
# so an iOS RELEASE build of an otherwise correctly configured QuickJS app ships
# a Hermes bytecode bundle. QuickJS cannot execute HBC. The app dies on launch.
# A debug build is unaffected, because it loads plain JavaScript from Metro and
# never runs this branch at all — which is exactly the asymmetry that let the
# equivalent Android bug (`hermesEnabled=true`) survive undetected until an
# actual release build was made on a device.
#
# WHAT THE TWO HELPERS DO
#
# `use_quickjs!` removes Hermes from the build. `react_native_quickjs_post_install`
# then writes the one build setting the bundle script reads. Both are needed:
# the first stops an 11 MB hermesvm.framework being installed, the second stops
# a release build compiling the bundle to Hermes bytecode.
#
# Note the post_install value is the STRING "false": react-native-xcode.sh
# compares against the literal text `false` in a shell `[[ ]]`, so `0`, `NO` and
# `FALSE` all read as "use Hermes".

# Removes Hermes. Call inside the target block, BEFORE use_react_native!: both
# the environment variable and the two overrides have to be in place while the
# podspecs are being evaluated, so post_install is far too late.
#
# THREE THINGS ADD HERMES, AND ALL THREE HAVE TO GO
#
# 1. Every podspec dependency on hermes-engine and React-hermes is already
#    behind `if use_hermes()` (React-jsi.podspec:41, React-utils.podspec:50,
#    React-RCTAppDelegate.podspec:65, and a dozen more). jsengine.rb defines
#    use_hermes() as `!use_third_party_jsc()`, which reads USE_THIRD_PARTY_JSC
#    and nothing else -- setting USE_HERMES has no effect on it. So the
#    environment variable below turns all of those off at once.
#
#    It also matters for a second reason: React-jsi.podspec drops jsi.cpp from
#    its own sources when use_hermes() is true, because hermes-engine supplies
#    it. Remove Hermes without flipping this and JSI itself fails to link.
#
# 2. `setup_hermes!` declares `pod 'hermes-engine'` and `pod 'React-hermes'` in
#    the Podfile directly, so neither goes through use_hermes(). It is called
#    from use_react_native! under `if hermes_enabled` -- but react_native_pods.rb
#    line 81 assigns `hermes_enabled = true` unconditionally, six lines into the
#    function and eleven lines after the parameter of the same name. The
#    parameter cannot turn it off; only replacing the function can.
#
# 3. `setup_bridgeless!` declares React-RuntimeHermes under a use_hermes:
#    parameter of its own, which use_react_native! feeds from that same
#    hardcoded true. It declares four other pods that are needed, so this one
#    is kept and called with that single argument forced.
#
# 4. `depend_on_js_engine` offers exactly two answers, hermes-engine or
#    React-jsc, with no "neither". Left alone it would make roughly fifteen
#    podspecs depend on a React-jsc pod that does not exist here --
#    @react-native-community/javascriptcore cannot be installed on 0.85, as it
#    still declares RCT-Folly, which 0.85 no longer ships. Replacing it is what
#    lets an app depend on no engine pod at all.
#
# React Native needs nothing from either engine once its own factory is
# replaced: under USE_THIRD_PARTY_JSC=1, RCTDefaultReactNativeFactoryDelegate's
# createJSRuntimeFactory compiles to an empty body and waits for the app to
# override it, which an app on QuickJS already does.
def use_quickjs!
  ENV['USE_THIRD_PARTY_JSC'] = '1'

  # React Native core has to be built from source. On the prebuilt path the
  # JavaScript Interface implementation itself lives in hermesvm.framework --
  # the prebuilt React.framework exports eleven facebook::jsi symbols and not
  # jsi::Value's destructor, hermesvm exports a hundred and fifteen including
  # it, and React-jsi.podspec drops its own jsi.cpp whenever Hermes is on. So
  # removing Hermes from the prebuilt path removes JSI with it, and every
  # runtime, ours included, fails to link. Built from source, React-jsi
  # compiles jsi.cpp itself.
  #
  # The cost is real and falls on the consumer: a first build and a cold CI
  # build compile React Native core rather than downloading it.
  ENV['RCT_USE_PREBUILT_RNCORE'] = '0'

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
  react_native_quickjs_set_cdp_for_debug(installer)
  react_native_quickjs_undefine_hermes(installer)
  react_native_quickjs_relax_return_type(installer)

  projects = installer.aggregate_targets
    .map { |t| t.user_project }
    .uniq { |p| p.path }
    .push(installer.pods_project)
    .compact

  projects.each do |project|
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

# The Chrome DevTools Protocol backend is compiled into debug configurations
# only -- the same gate React Native puts on its own inspector, in
# scripts/cocoapods/utils.rb's set_gcc_preprocessor_definition_for_debugger.
#
# QuickJSInstance::debuggerEnabledByDefault() already refuses to attach a
# debugger in a release build, so without this the release binary would carry a
# backend nothing can reach. Keyed on config.type rather than the name, so an
# app whose debug configuration is called something else still gets it.
def react_native_quickjs_set_cdp_for_debug(installer)
  react_native_quickjs_add_define(
    installer, "ReactNativeQuickJS", "RNQJS_ENABLE_CDP=1", :debug_only => true
  )
end

# RCTCxxBridge.mm guards its Hermes import with
#
#   #if !defined(USE_HERMES) || USE_HERMES == 1
#
# and jsengine.rb's js_engine_flags() returns "-DUSE_THIRD_PARTY_JSC=1" for the
# non-Hermes case -- it never defines USE_HERMES at all. So the guard's first
# clause holds, the file imports reacthermes/HermesExecutorFactory.h, and
# React-Core fails to compile once Hermes is gone. Defining it as 0 is what the
# guard was written to expect.
#
# React-RCTAppDelegate needs the opposite repair. RCTAppSetupUtils.h guards the
# same import with `#if USE_THIRD_PARTY_JSC != 1`, and the flag that would
# satisfy it is lost to a missing space in React-RCTAppDelegate.podspec:21:
#
#   new_arch_enabled_flag = " -DRCT_NEW_ARCH_ENABLED=1"   <- no trailing space
#   other_cflags = "$(inherited) " + new_arch_enabled_flag + js_engine_flags()
#
# which clang receives as the single define
#
#   -DRCT_NEW_ARCH_ENABLED=1-DUSE_THIRD_PARTY_JSC=1
#
# so USE_THIRD_PARTY_JSC is never defined. With Hermes present this is
# invisible, because the guard then falls through to an import that exists.
def react_native_quickjs_undefine_hermes(installer)
  react_native_quickjs_add_define(installer, "React-Core", "USE_HERMES=0")
  react_native_quickjs_add_define(
    installer, "React-RCTAppDelegate", "USE_THIRD_PARTY_JSC=1"
  )
end

# RCTDefaultReactNativeFactoryDelegate's createJSRuntimeFactory is
#
#   - (JSRuntimeFactoryRef)createJSRuntimeFactory
#   {
#   #if USE_THIRD_PARTY_JSC != 1
#     return jsrt_create_hermes_factory();
#   #endif
#   }
#
# so under this flag the body is empty and -Werror,-Wreturn-type rejects it.
# React Native's own non-Hermes configuration does not compile.
#
# The sharp edge this leaves: an app that does NOT override
# createJSRuntimeFactory now falls off the end of a non-void function instead
# of failing to build. An app on QuickJS overrides it -- that override is how
# it selects QuickJS at all -- so the path is unreachable in a working app.
def react_native_quickjs_relax_return_type(installer)
  installer.target_installation_results.pod_target_installation_results
    .each do |name, result|
      next unless name.to_s == "React-RCTAppDelegate"
      result.native_target.build_configurations.each do |config|
        f = config.build_settings["OTHER_CFLAGS"] || "$(inherited)"
        f = f.join(" ") if f.is_a?(Array)
        config.build_settings["OTHER_CFLAGS"] = "#{f} -Wno-error=return-type"
      end
    end
end

def react_native_quickjs_add_define(installer, pod_name, define, debug_only: false)
  installer.target_installation_results.pod_target_installation_results
    .each do |name, result|
      next unless name.to_s == pod_name

      result.native_target.build_configurations.each do |config|
        next if debug_only && config.type != :debug

        defs = config.build_settings["GCC_PREPROCESSOR_DEFINITIONS"] || "$(inherited)"
        defs = defs.join(" ") if defs.is_a?(Array)
        config.build_settings["GCC_PREPROCESSOR_DEFINITIONS"] = "#{defs} #{define}"
      end
    end
end
