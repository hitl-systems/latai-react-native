# SPDX-License-Identifier: Apache-2.0 OR MIT
# The iOS side of latai-react-native. Put the xcframework of the mobile build (phase 14.1) at
# ios/Frameworks/latai.xcframework (a static or dynamic library; not committed).
require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "latai-react-native"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = "https://github.com/lat-one/lat-one"
  s.license      = { :type => "Apache-2.0 OR MIT" }
  s.author       = { "latai" => "noreply@example.invalid" }
  s.platforms    = { :ios => "15.0" } # liblatai is built for iOS 15 (docs/mobile.md)
  s.source       = { :path => "." }

  s.source_files = "ios/**/*.{h,m,mm}", "cpp/LataiJsi.{h,cpp}"
  s.vendored_frameworks = "ios/Frameworks/latai.xcframework"
  # The static library carries the C++ engine: the app link needs libc++ and the frameworks that
  # llama-cpp-sys-2 names (Metal is not used; the sys crate links it anyway).
  s.libraries = "c++"
  s.frameworks = "Foundation", "Accelerate", "Metal", "MetalKit"
  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++17",
    # cpp/ for LataiJsi.h, include/ for latai.h (the package's own copy, kept by check_bindings.py)
    "HEADER_SEARCH_PATHS" => "\"$(PODS_TARGET_SRCROOT)/cpp\" \"$(PODS_TARGET_SRCROOT)/include\"",
  }

  if respond_to?(:install_modules_dependencies, true)
    install_modules_dependencies(s)
  else
    s.dependency "React-Core"
  end
  s.dependency "React-callinvoker"
  s.dependency "React-jsi"
end
