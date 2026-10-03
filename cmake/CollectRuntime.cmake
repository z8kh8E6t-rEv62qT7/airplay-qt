cmake_minimum_required(VERSION 3.25)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()
function(airplay_collect_runtime binary platform destination)
  get_filename_component(name "${binary}" NAME)
  string(SHA256 key "${binary}")
  set(scan "${SCAN_ROOT}/${key}")
  file(MAKE_DIRECTORY "${scan}")
  file(COPY "${binary}" DESTINATION "${scan}")
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL dumpbin)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${DUMPBIN}")
  set(ENV{PATH} "${DEPENDENCY_BIN};${QT_BIN};$ENV{SystemRoot}/System32")
  file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES "${scan}/${name}" "${platform}"
    DIRECTORIES "${DEPENDENCY_BIN}" "${QT_BIN}" "${PLIST_BIN}"
    PRE_EXCLUDE_REGEXES "[Aa][Pp][Ii]-[Mm][Ss]-" "[Ee][Xx][Tt]-[Mm][Ss]-"
    POST_EXCLUDE_REGEXES "[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/[Ss][Yy][Ss][Tt][Ee][Mm]32/"
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR missing
    CONFLICTING_DEPENDENCIES_PREFIX conflicts)
  if(missing OR conflicts_FILENAMES)
    message(FATAL_ERROR "Runtime unresolved: ${missing}; conflicting: ${conflicts_FILENAMES}")
  endif()
  file(MAKE_DIRECTORY "${destination}/platforms")
  set(checksums "")
  set(manifest "")
  foreach(dll IN LISTS dependencies)
    get_filename_component(filename "${dll}" NAME)
    file(COPY_FILE "${dll}" "${destination}/${filename}" ONLY_IF_DIFFERENT)
    file(SHA256 "${dll}" hash)
    string(APPEND checksums "${hash}  ${filename}\n")
    string(APPEND manifest "${dll}\n")
  endforeach()
  file(COPY_FILE "${platform}" "${destination}/platforms/qwindows.dll" ONLY_IF_DIFFERENT)
  file(SHA256 "${platform}" hash)
  string(APPEND checksums "${hash}  platforms/qwindows.dll\n")
  file(SHA256 "${binary}" hash)
  string(APPEND checksums "${hash}  ${name}")
  # MSYS2 checksum verification and payload copying require LF-delimited paths.
  # Substitute whole values once so path text is not treated as a template;
  # file(CONFIGURE) supplies each file's final newline.
  file(CONFIGURE OUTPUT "${destination}/runtime-sha256.txt"
    CONTENT "@checksums@" @ONLY NEWLINE_STYLE LF)
  file(CONFIGURE OUTPUT "${destination}/runtime-dependencies.txt"
    CONTENT "@manifest@@platform@\n@binary@" @ONLY NEWLINE_STYLE LF)
endfunction()
