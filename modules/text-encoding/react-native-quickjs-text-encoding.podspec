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
  s.name         = "react-native-quickjs-text-encoding"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.license      = package["license"]
  s.author       = package["author"]
  s.homepage     = package["homepage"] || "https://example.com"
  s.platforms    = { :ios => "15.1" }
  s.source       = { :git => ".", :tag => "#{s.version}" }

  # The C++ headers are not API for consumers -- registration happens through
  # the textEncoding_install symbol -- so they are private. CocoaPods generates
  # a module map for any pod with Swift sources, and its umbrella header is
  # built from the *public* headers; leaving these public would put jsi.h and
  # quickjs.h inside a Clang module that Swift then has to compile, which fails
  # (C++ headers are not importable from Swift). Private keeps them out of the
  # umbrella while still visible to the pod's own sources.
  s.private_header_files = "cpp/**/*.h"

  # Compiled from source into the app, alongside the engine itself. The ios/
  # sources are the Apple half of the platform seam — Foundation lives there,
  # so a module can use NSLocale, Security.framework and the rest without
  # linking anything extra.
  s.source_files = "cpp/**/*.{h,hpp,c,cpp}", "ios/**/*.{h,m,mm,swift}"

  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
    # The engine headers and the module ABI live in the react-native-quickjs pod.
    "HEADER_SEARCH_PATHS" =>
      "\"#{rnqjs_root}/src/bytecode\" " \
      "\"#{rnqjs_root}/src/module\" " \
      "\"#{rnqjs_root}/src/runtime\" " \
      "\"#{rnqjs_root}/engine/quickjs-rel\"",
  }

  s.dependency "React-jsi"
  s.dependency "React-Core"
  s.dependency "ReactNativeQuickJS"
end
