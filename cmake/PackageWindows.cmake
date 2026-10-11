# Package only product targets; test executables stay in the build tree.
configure_file("${CMAKE_CURRENT_LIST_DIR}/PackageWindowsRun.cmake.in"
  "${PROJECT_BINARY_DIR}/PackageWindowsRun.cmake" @ONLY)
add_custom_target(package-windows
  COMMAND "${CMAKE_COMMAND}"
    "-DAPP=$<TARGET_FILE:AirPlayQt>" "-DPLUGIN=$<TARGET_FILE:AirPlayQtVst3>"
    "-DENGINE=$<TARGET_FILE:AirPlayQtEngine>" "-DQT_BIN=$<TARGET_FILE_DIR:Qt6::Core>"
    "-DMODULEINFO=$<TARGET_FILE:moduleinfotool>" "-DCONFIG=$<CONFIG>"
    -P "${PROJECT_BINARY_DIR}/PackageWindowsRun.cmake"
  DEPENDS AirPlayQt AirPlayQtVst3 AirPlayQtEngine moduleinfotool
  USES_TERMINAL VERBATIM)
