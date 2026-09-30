# Reuse the C ABI of the CLANG64 DLL; never link its C++ wrapper or static archive.
set(LIBPLIST_ROOT "E:/MSYS/clang64" CACHE PATH "Existing libplist 2.7+ C DLL installation")
find_path(LIBPLIST_INCLUDE_DIR plist/plist.h PATHS "${LIBPLIST_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
find_file(LIBPLIST_DLL libplist-2.0.dll PATHS "${LIBPLIST_ROOT}/bin" NO_DEFAULT_PATH REQUIRED)
set(plist_import_dir "${CMAKE_BINARY_DIR}/libplist")
file(MAKE_DIRECTORY "${plist_import_dir}")
# Expose only this C header, not the MinGW CRT headers in its parent directory.
configure_file("${LIBPLIST_INCLUDE_DIR}/plist/plist.h"
  "${plist_import_dir}/include/plist/plist.h" COPYONLY)
get_filename_component(msvc_tools_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(LIBPLIST_DUMPBIN dumpbin.exe PATHS "${msvc_tools_dir}" NO_DEFAULT_PATH REQUIRED)
find_program(LIBPLIST_LIB_TOOL lib.exe PATHS "${msvc_tools_dir}" NO_DEFAULT_PATH REQUIRED)
execute_process(COMMAND "${LIBPLIST_DUMPBIN}" /exports "${LIBPLIST_DLL}"
  OUTPUT_VARIABLE plist_exports RESULT_VARIABLE plist_result)
if(NOT plist_result EQUAL 0)
  message(FATAL_ERROR "Cannot read libplist DLL exports")
endif()
string(REGEX MATCHALL "[\r\n]+ +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +[A-Za-z_][A-Za-z_0-9]*" plist_lines "${plist_exports}")
set(plist_def "LIBRARY libplist-2.0.dll\nEXPORTS\n")
foreach(line IN LISTS plist_lines)
  string(REGEX MATCH "[A-Za-z_][A-Za-z_0-9]*$" symbol "${line}")
  string(APPEND plist_def "${symbol}\n")
endforeach()
if(NOT plist_def MATCHES "plist_mem_free\n" OR NOT plist_def MATCHES "plist_new_int\n")
  message(FATAL_ERROR "libplist DLL lacks required 2.7 C API exports")
endif()
file(WRITE "${plist_import_dir}/libplist.def" "${plist_def}")
execute_process(COMMAND "${LIBPLIST_LIB_TOOL}" /nologo /machine:x64
  "/def:${plist_import_dir}/libplist.def" "/out:${plist_import_dir}/libplist.lib"
  RESULT_VARIABLE plist_result)
if(NOT plist_result EQUAL 0)
  message(FATAL_ERROR "Cannot generate MSVC libplist import library")
endif()
add_library(LibPlist::LibPlist SHARED IMPORTED)
set_target_properties(LibPlist::LibPlist PROPERTIES
  IMPORTED_IMPLIB "${plist_import_dir}/libplist.lib"
  IMPORTED_LOCATION "${LIBPLIST_DLL}"
  INTERFACE_INCLUDE_DIRECTORIES "${plist_import_dir}/include")
configure_file("${LIBPLIST_DLL}" "${plist_import_dir}/libplist-2.0.dll" COPYONLY)
