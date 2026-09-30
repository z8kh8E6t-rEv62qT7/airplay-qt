# Packaging is explicit: normal developer builds remain fast and unpackaged.
find_program(AIRPLAY_MACDEPLOYQT macdeployqt HINTS "${Qt6_DIR}/../../../bin" REQUIRED)
get_target_property(package_plugin AirPlayQtVst3 SMTG_PLUGIN_PACKAGE_PATH)
add_executable(test_macos_app tests/app/MacAppSmoke.mm)
set_target_properties(test_macos_app PROPERTIES AUTOMOC OFF)
target_link_libraries(test_macos_app PRIVATE "-framework Cocoa")
configure_file("${CMAKE_CURRENT_LIST_DIR}/PackageMacRun.cmake.in"
  "${PROJECT_BINARY_DIR}/PackageMacRun.cmake" @ONLY)
add_custom_target(package-macos
  COMMAND "${CMAKE_COMMAND}"
    "-DAPP=$<TARGET_BUNDLE_DIR:AirPlayQt>" "-DPLUGIN=${package_plugin}"
    "-DHOST=$<TARGET_FILE:test_vst3_module>" "-DSMOKE=$<TARGET_FILE:test_macos_app>"
    "-DVALIDATOR=$<TARGET_FILE:validator>" "-DCONFIG=$<CONFIG>"
    -P "${PROJECT_BINARY_DIR}/PackageMacRun.cmake"
  DEPENDS AirPlayQt AirPlayQtVst3 test_vst3_module test_macos_app validator
    test_airplay test_discovery test_coreaudio test_vst3
  USES_TERMINAL VERBATIM)
