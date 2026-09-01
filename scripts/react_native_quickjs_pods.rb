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
# WHAT THIS HELPER DOES INSTEAD
#
# It writes the ONE build setting the bundle script actually reads, after React
# Native has written its own value. No environment variable, so
# `error_if_try_to_use_jsc_from_core` never fires, and no dependency on
# `USE_THIRD_PARTY_JSC=1`, which would pull in React-jsc and put a *second*
# engine in the app — the opposite of the point.
#
# Note the value is the STRING "false": react-native-xcode.sh compares against
# the literal text `false` in a shell `[[ ]]`, so `0`, `NO` and `FALSE` all read
# as "use Hermes".

def react_native_quickjs_post_install(installer)
  react_native_quickjs_set_cdp_for_debug(installer)

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
  installer.target_installation_results.pod_target_installation_results
    .each do |pod_name, result|
      next unless pod_name.to_s == "ReactNativeQuickJS"

      result.native_target.build_configurations.each do |config|
        next unless config.type == :debug

        defs = config.build_settings["GCC_PREPROCESSOR_DEFINITIONS"] || "$(inherited)"
        defs = defs.join(" ") if defs.is_a?(Array)
        config.build_settings["GCC_PREPROCESSOR_DEFINITIONS"] =
          "#{defs} RNQJS_ENABLE_CDP=1"
      end
    end
end
