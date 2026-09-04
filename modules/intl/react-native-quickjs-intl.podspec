require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

# --- locating the engine package -------------------------------------------
#
# This module compiles against the engine's headers (src/ for the module ABI,
# engine/quickjs-rel/ for quickjs itself), so it has to know where
# @react-native-quickjs/quickjs actually is.
#
# That path used to be hardcoded as
# "$(PODS_ROOT)/../../node_modules/react-native-quickjs", which was wrong three
# ways. The package is scoped, so that directory does not exist under the name
# it assumed. A monorepo, a yarn `nohoist`, or pnpm's layout puts node_modules
# somewhere else entirely. And because it was a build-setting string rather than
# something evaluated at install time, being wrong surfaced as a missing-header
# error in the middle of a compile instead of at `pod install`, where the cause
# is one line away.
#
# Resolved with node, the same way android/build.gradle already resolves
# react-native.
rnqjs_root = begin
  script = "require.resolve('@react-native-quickjs/quickjs/package.json', " \
           "{paths: [process.argv[1]]})"
  out = IO.popen(
    ["node", "--print", script, __dir__], :err => File::NULL, &:read
  ).to_s.strip

  if !out.empty?
    File.dirname(out)
  else
    # In THIS repository the engine is the workspace root, and yarn does not
    # link a workspace root into its own node_modules -- so require.resolve
    # genuinely cannot see it and this is the normal path for local development,
    # not an error case. Walk up looking for the right package.json.
    dir = __dir__
    found = nil
    while found.nil? && dir != "/"
      manifest = File.join(dir, "package.json")
      if File.exist?(manifest) &&
         JSON.parse(File.read(manifest))["name"] == "@react-native-quickjs/quickjs"
        found = dir
      end
      dir = File.dirname(dir)
    end
    found
  end
end

if rnqjs_root.nil?
  raise <<~MSG
    [#{File.basename(__FILE__, ".podspec")}] cannot find @react-native-quickjs/quickjs.

    This module compiles against the engine's headers and cannot build without
    it. Install it alongside this module:

      yarn add @react-native-quickjs/quickjs
  MSG
end

Pod::Spec.new do |s|
  s.name         = "react-native-quickjs-intl"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.license      = package["license"]
  s.author       = package["author"]
  s.homepage     = package["homepage"] || "https://example.com"
  s.platforms    = { :ios => "15.1" }
  s.source       = { :git => ".", :tag => "#{s.version}" }

  # Compiled from source into the app, alongside the engine itself. The ios/
  # sources are the Apple half of the platform seam — Foundation lives there,
  # so NSLocale, NSDateFormatter and NSTimeZone are reachable without linking
  # ICU or shipping one byte of CLDR data.
  #
  # The .swift file is IntlLikelySubtags.swift and nothing else. Adding Swift to
  # a pod changes its linkage requirements and is the largest build-integration
  # risk in the module, so the Swift surface is exactly six @_cdecl functions,
  # each declared __attribute__((weak)) on the Objective-C++ side. A build in
  # which Swift does not link still links, still runs, and degrades to "no
  # opinion" on the six things only Swift can answer.
  #
  # The C++ headers are not API for consumers -- registration happens through
  # the intl_install symbol -- so they are private. CocoaPods generates a
  # module map for any pod with Swift sources, and its umbrella header is built
  # from the *public* headers; leaving these public would put jsi.h and
  # quickjs.h inside a Clang module that Swift then has to compile, which fails
  # (C++ headers are not importable from Swift). Private keeps them out of the
  # umbrella while still visible to the pod's own sources.
  s.private_header_files = "cpp/**/*.h"

  s.source_files = "cpp/**/*.{h,hpp,c,cpp}", "ios/**/*.{h,m,mm,swift}"

  # ARC, stated rather than inherited. CocoaPods defaults this to true, so the
  # pod already had it — but the *host* CMake build did not, and the two
  # therefore compiled different programs. Anything that measures this module's
  # memory must measure the ARC build, which is this one.
  s.requires_arc = true

  # Generated headers land in a build-directory subfolder that the script phase
  # below creates. They are not in source control: quickjs bytecode is loadable
  # only by the engine build that produced it, so a committed blob is a trap the
  # first time the vendored engine moves.
  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
    "SWIFT_VERSION" => "5.0",
    "HEADER_SEARCH_PATHS" =>
      "\"#{rnqjs_root}/src/bytecode\" " \
      "\"#{rnqjs_root}/src/module\" " \
      "\"#{rnqjs_root}/src/runtime\" " \
      "\"#{rnqjs_root}/engine/quickjs-rel\" " \
      '"$(PODS_TARGET_SRCROOT)/cpp" ' \
      '"$(DERIVED_FILE_DIR)/rnqjs-intl"',
  }

  # Build-time compilation of js/intl.js to bytecode.
  #
  # The compiler must be a qjsc built from the *same* engine revision as the
  # runtime, and an iOS build cannot run the one it builds for the device. The
  # engine package ships a per-host compiler under bin/qjsc/<host>/; this build
  # runs on macOS, so it looks there. If none is found, embed-js.js emits the
  # source instead and the module compiles it on first use of Intl — still
  # lazy, still correct, but it pays the parse.
  s.script_phases = [{
    :name => "Embed Intl JavaScript",
    :script => <<~SCRIPT,
      set -e
      MODULE_DIR="${PODS_TARGET_SRCROOT}"
      RNQJS_DIR="#{rnqjs_root}"
      OUT="${DERIVED_FILE_DIR}/rnqjs-intl"
      mkdir -p "$OUT"
      case "$(uname -m)" in
        x86_64) trip="darwin-x64" ;;
        arm64) trip="darwin-arm64" ;;
        *) trip="" ;;
      esac
      QJSC=""
      [ -n "$trip" ] && [ -x "$RNQJS_DIR/bin/qjsc/$trip/qjsc" ] \
        && QJSC="$RNQJS_DIR/bin/qjsc/$trip/qjsc"
      if [ -n "$QJSC" ]; then
        node "$MODULE_DIR/scripts/embed-js.js" --out "$OUT" --qjsc "$QJSC"
      else
        echo "warning: no host qjsc found; embedding Intl JS as source (adds a parse on first Intl use)"
        node "$MODULE_DIR/scripts/embed-js.js" --out "$OUT"
      fi
    SCRIPT
    :execution_position => :before_compile,
    :input_files => ["${PODS_TARGET_SRCROOT}/js/intl.js"],
    :output_files => [
      "${DERIVED_FILE_DIR}/rnqjs-intl/IntlSource.h",
      "${DERIVED_FILE_DIR}/rnqjs-intl/IntlBlob.h",
      "${DERIVED_FILE_DIR}/rnqjs-intl/IntlBuildConfig.h",
    ],
  }]

  s.dependency "React-jsi"
  s.dependency "React-Core"
  s.dependency "ReactNativeQuickJS"
end
