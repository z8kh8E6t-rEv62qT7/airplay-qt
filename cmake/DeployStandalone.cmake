# Deploy the standalone application only. VST3 hosting has its own environment.
get_filename_component(deploy_prefix "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
find_file(AIRPLAY_QWINDOWS qwindows.dll
  PATHS "${deploy_prefix}/lib/qt6/plugins/platforms" NO_DEFAULT_PATH REQUIRED)
configure_file("${CMAKE_CURRENT_LIST_DIR}/DeployStandaloneRuntime.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/DeployStandaloneRuntime.cmake" @ONLY)
add_custom_target(AirPlayQtRuntime ALL
  COMMAND "${CMAKE_COMMAND}" "-DAPP=$<TARGET_FILE:AirPlayQt>"
    "-DQT_BIN=$<TARGET_FILE_DIR:Qt6::Core>"
    -P "${CMAKE_CURRENT_BINARY_DIR}/DeployStandaloneRuntime.cmake"
  DEPENDS AirPlayQt
  COMMENT "Deploying standalone DLLs and Qt Windows platform plugin"
  VERBATIM)
