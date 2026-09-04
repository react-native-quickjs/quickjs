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

require "json"
require "pathname"

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
def use_quickjs!
  # Turns off every `if use_hermes()` dependency on hermes-engine at once.
  # React Native's own use_hermes() is `!use_third_party_jsc()`, so this one
  # flag answers for every React Native pod.
  ENV['USE_THIRD_PARTY_JSC'] = '1'

  # Set as well, because a podspec is free to read it directly rather than call
  # use_hermes() -- Expo's does -- and to such a reader an unset USE_HERMES
  # means "yes, Hermes". Leaving it unset would have this app claim an engine it
  # does not have, and any library added later would be told the same.
  #
  # Safe alongside the flag above: error_if_try_to_use_jsc_from_core aborts on
  # USE_HERMES=0 only while USE_THIRD_PARTY_JSC is unset or 0, which is the
  # "asking for the JavaScriptCore that used to be in core" case, not this one.
  #
  # A reader that has only two engines in mind resolves this to JavaScriptCore
  # rather than to no engine. That is what the Expo config plugin's podspec
  # patch is for: it teaches USE_THIRD_PARTY_JSC as a third answer.
  ENV['USE_HERMES'] = '0'

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
  react_native_quickjs_add_module_registry(installer)

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

# Generated module registry, compiled into the app.
#
# WHY
#   A QuickJS module registers itself through QJS_REGISTER_MODULE, a static
#   initializer in its translation unit. That works for a shared library (a
#   .so is loaded whole and runs every initializer) but not for a static
#   archive: the linker drops any object file nothing references, so on iOS a
#   module's object -- and its registration -- silently vanishes and the global
#   it installs (Intl, TextEncoder) is undefined. The generated registry names
#   each module's install function explicitly, which is a direct reference the
#   linker cannot drop, and installModules() calls registerGeneratedModules()
#   before installing anything.
#
# WHY THE APP TARGET AND NOT THE ENGINE POD
#   A static linker resolves archives left to right, and CocoaPods orders each
#   module pod before the engine pod it depends on. The registry's reference to
#   a module's install symbol must therefore live in the *last*-linked thing --
#   the app target -- not in the engine archive, or the reference would arrive
#   after the module archive had already been passed over and the link would
#   fail with an undefined symbol.
#
# HOW
#   scripts/generate-module-registry.js names exactly the installed quickjs
#   module pods (--dir is authoritative, so a package sitting in node_modules
#   without a linked native build is never referenced -- that would be an
#   undefined symbol at link). The generated translation unit is added to the
#   app target's compile sources, a build phase regenerates it before compile,
#   and the app target is given the engine's module-ABI header path so the file
#   compiles there.
#
# The host build exercises the same path in quickjs_generated_modules_tests.
MODULE_REGISTRY_PHASE = "[ReactNativeQuickJS] Generate module registry"

def react_native_quickjs_add_module_registry(installer)
  engine_root = File.expand_path("..", __dir__)
  # Resolved at build time relative to $SRCROOT via node, so the checked-in
  # project carries no absolute path: the script asks node where the engine is
  # (require.resolve) exactly like the bytecode phase above does.
  generator = "scripts/generate-module-registry.js"

  sandbox = Pod::Config.instance.sandbox
  sandbox_root = sandbox.root.to_s

  # An installed quickjs-module pod is named react-native-quickjs-<segment>. Its
  # package.json (which declares the reactNativeQuickJSModule field) sits next
  # to its podspec, wherever that is -- node_modules/@react-native-quickjs/ for
  # a consumer, modules/ for this monorepo. The absolute package directory comes
  # from the local-pod storage CocoaPods keeps (keyed by pod name), which is the
  # original `:path` for a file: dependency -- so no layout assumption here.
  module_dirs = []
  dev_pods = sandbox.send(:development_pods)
  (installer.pod_targets || []).each do |pod_target|
    next unless pod_target.name.start_with?("react-native-quickjs-")
    # For a local (file:) pod, development_pods stores the podspec file path.
    # Its directory is the module's package root (package.json sits next to the
    # podspec). Prefer it over any layout guess.
    spec_file = dev_pods[pod_target.pod_name].to_s
    pkg_root = spec_file.empty? ? "" : File.dirname(spec_file)
    if pkg_root.empty? || !File.exist?(File.join(pkg_root, "package.json"))
      spec_file = pod_target.root_spec.defined_in_file
      pkg_root = spec_file ? File.dirname(spec_file) : ""
    end
    next if pkg_root.empty?
    pkg = File.join(pkg_root, "package.json")
    next unless File.exist?(pkg)
    next unless JSON.parse(File.read(pkg))["reactNativeQuickJSModule"]
    module_dirs << pkg_root
  end

  # QuickJS modules that failed the reactNativeQuickJSModule check above are not
  # interesting; but a pod can also be excluded from pod_targets entirely (e.g.
  # script_phase-only). Detect by name from the pods project so the registry is
  # not silently empty when modules ARE installed.
  if module_dirs.empty? && (installer.pods_project)
    (installer.pods_project.targets).each do |t|
      next unless t.name.start_with?("react-native-quickjs-")
      Pod::UI.warn(
        "[ReactNativeQuickJS] found pod #{t.name} but no package.json with " \
        "reactNativeQuickJSModule next to it; module registry will be empty."
      )
    end
  end

  return if module_dirs.empty?

  node_binary = ENV["NODE_BINARY"] || "node"

  # The generated file includes src/module/QuickJSModule.h (which includes
  # <jsi/jsi.h>). The module pod exposes those headers to itself, not to the app
  # target, so the app target needs the engine's module-ABI header path added to
  # compile the registry. The path is resolved here with node (the same way the
  # module podspecs resolve the engine) and written into the project -- which is
  # exactly what the engine pod's own xcconfig already does for its headers.
  jsi_headers = File.join(sandbox_root, "Headers", "Public", "React-jsi")
  engine_src = File.join(engine_root, "src", "module")
  engine_headers = [
    "\"#{jsi_headers}\"",
    "\"#{engine_src}\"",
  ].select { |p| p.include?("$(PODS_ROOT)") || File.directory?(p.tr("\"", "")) }

  installer.aggregate_targets.map(&:user_project).uniq(&:path).compact.each do |project|
    project.native_targets.each do |target|
      next unless target.product_type == "com.apple.product-type.application"

      project_dir = File.dirname(project.path)
      # The generated translation unit lives in the app project's build dir. It
      # is referenced from the .xcodeproj and produced by the script phase using
      # only paths Xcode already knows ($SRCROOT is the directory holding the
      # .xcodeproj), so nothing machine-specific is written into the project.
      out = File.join(project_dir, "build", "generated", "QuickJSGeneratedModules.cpp")

      # Header search paths for the generated translation unit. Built from
      # build settings only, so nothing machine-specific reaches the project.
      target.build_configurations.each do |config|
        paths = [config.build_settings["HEADER_SEARCH_PATHS"] || "$(inherited)"]
        paths = [paths] unless paths.is_a?(Array)
        paths = paths.flatten
        paths.concat(engine_headers)
        config.build_settings["HEADER_SEARCH_PATHS"] = paths.uniq.join(" ")
      end

      # Idempotent: reuse an existing phase (a re-run of this pod helper must
      # refresh it, not accumulate another), but keep it ordered before Sources.
      phase = target.build_phases.find do |p|
        p.respond_to?(:display_name) && p.display_name == MODULE_REGISTRY_PHASE
      end
      phase ||= target.new_shell_script_build_phase(MODULE_REGISTRY_PHASE)

      # Regenerate on every build, before sources compile. The generated file is
      # declared as an output so the build system knows this phase produces the
      # source that Compile Sources consumes (otherwise it errors with "Build
      # input file cannot be found"). The engine and the module directories are
      # resolved at build time through node, and the output is addressed with
      # $SRCROOT, so nothing machine-specific lands in the checked-in project.
      phase.output_paths = ["$(SRCROOT)/build/generated/QuickJSGeneratedModules.cpp"]
      rel_dirs = module_dirs.map do |d|
        Pathname.new(d).relative_path_from(Pathname.new(engine_root)).to_s
      end
      phase.shell_script = <<~SH
        set -e
        [ -f "$SRCROOT/.xcode.env" ] && . "$SRCROOT/.xcode.env"
        [ -f "$SRCROOT/.xcode.env.local" ] && . "$SRCROOT/.xcode.env.local"
        : "${NODE_BINARY:=#{node_binary}}"
        ENGINE="$("$NODE_BINARY" --print "require.resolve('@react-native-quickjs/quickjs/package.json', {paths: ['$SRCROOT']})" | xargs dirname)"
        OUT="$SRCROOT/build/generated/QuickJSGeneratedModules.cpp"
        mkdir -p "$(dirname "$OUT")"
        "$NODE_BINARY" "$ENGINE/#{generator}" --dir "$ENGINE/#{rel_dirs.join('" --dir "$ENGINE/')}" \\
          --out "$OUT" --quiet
      SH

      compile = target.build_phases.find do |p|
        p.respond_to?(:display_name) && p.display_name == "Sources"
      end
      target.build_phases.delete(phase)
      index = compile ? target.build_phases.index(compile) : nil
      target.build_phases.insert(index, phase) if index

      rel = Pathname.new(out).relative_path_from(Pathname.new(project_dir)).to_s
      ref = project.main_group.files.find { |f| f.path == rel }
      ref ||= project.main_group.new_file(rel)
      target.source_build_phase.add_file_reference(ref)

      Pod::UI.puts(
        "[ReactNativeQuickJS] module registry -> #{module_dirs.map { |d| File.basename(d) }.join(', ')}".green
      )
    end
    project.save
  end
end
