# =============================================================================
#  CMakeLists.txt - MinecraftSA.asi for GTA San Andreas (32-bit) using plugin-sdk
#
#  Repository layout (everything loose in the root, no source subfolders):
#      CMakeLists.txt
#      Minecraftsa.cpp
#      Elytraphysics.h
#      plugin-sdk.zip              <- zipped plugin-sdk (GitHub "Download ZIP" is fine)
#      MinecraftSA.ini             <- optional, copied next to the .asi
#      .github/workflows/build.yml <- CI (GitHub only reads workflows from here)
#
#  Local build (Developer PowerShell, 32-bit):
#      cmake -S . -B build -A Win32
#      cmake --build build --config Release --parallel
#  Result: build/dist/Release/MinecraftSA.asi
#
#  How plugin-sdk is found, in order:
#      1. -DPLUGIN_SDK_DIR=<folder>          (the CI workflow does this)
#      2. environment variable PLUGIN_SDK_DIR
#      3. plugin-sdk.zip in the repo root, extracted into the build folder
#  The folder may be the SDK root itself or hold it one or two levels down
#  (GitHub zips add a top-level "plugin-sdk-master/").
#
#  How plugin-sdk is linked:
#      - If the SDK tree contains a prebuilt plugin.lib (and optionally plugin_d.lib), it is used.
#      - Otherwise the SDK sources (plugin_sa/ and shared/) are compiled into a static library.
#        Force one or the other with -DPLUGIN_SDK_BUILD_FROM_SOURCE=ON|OFF (default AUTO).
# =============================================================================
cmake_minimum_required(VERSION 3.21)

# Static CRT: the .asi then needs no VC++ redistributable next to gta_sa.exe.
# Must be set before project(). A prebuilt plugin.lib must use the same runtime (/MT).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(MinecraftSA LANGUAGES CXX)

option(MINECRAFTSA_ALLOW_ANY_TOOLCHAIN
       "Skip the MSVC / 32-bit checks (only to smoke-test this CMake logic on another OS)" OFF)

if(NOT MINECRAFTSA_ALLOW_ANY_TOOLCHAIN)
  if(NOT MSVC)
    message(FATAL_ERROR "MinecraftSA must be built with MSVC (Visual Studio). Detected: ${CMAKE_CXX_COMPILER_ID}")
  endif()
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "GTA San Andreas is a 32-bit game. Configure with:  cmake -S . -B build -A Win32")
  endif()
endif()

set(CMAKE_CXX_STANDARD 17)            # Minecraftsa.cpp uses std::clamp
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PLUGIN_SDK_DIR "" CACHE PATH
    "Folder with the extracted plugin-sdk (contains plugin_sa/ and shared/). Empty = auto-detect.")
set(PLUGIN_SDK_GAME_VERSION "PLUGIN_SGV_10US" CACHE STRING
    "Game executable the plugin targets (plugin-sdk SGV define). Must match your gta_sa.exe.")
set_property(CACHE PLUGIN_SDK_GAME_VERSION PROPERTY STRINGS
             PLUGIN_SGV_10US PLUGIN_SGV_10EU PLUGIN_SGV_11US PLUGIN_SGV_11EU)
set(PLUGIN_SDK_BUILD_FROM_SOURCE "AUTO" CACHE STRING
    "AUTO = use prebuilt plugin.lib if the SDK has one, else compile the SDK sources. ON / OFF to force.")
set_property(CACHE PLUGIN_SDK_BUILD_FROM_SOURCE PROPERTY STRINGS AUTO ON OFF)

# -----------------------------------------------------------------------------
#  1. Locate (or extract) plugin-sdk
# -----------------------------------------------------------------------------
set(_sdk_search "${PLUGIN_SDK_DIR}")
if(NOT _sdk_search AND DEFINED ENV{PLUGIN_SDK_DIR})
  set(_sdk_search "$ENV{PLUGIN_SDK_DIR}")
endif()

if(NOT _sdk_search)
  set(_sdk_zip      "${CMAKE_CURRENT_SOURCE_DIR}/plugin-sdk.zip")
  set(_sdk_unzipped "${CMAKE_BINARY_DIR}/plugin-sdk-extracted")
  if(NOT EXISTS "${_sdk_zip}")
    message(FATAL_ERROR
      "plugin-sdk not found. Do one of:\n"
      "  - put plugin-sdk.zip in the repository root (${CMAKE_CURRENT_SOURCE_DIR})\n"
      "  - pass -DPLUGIN_SDK_DIR=<extracted folder>\n"
      "  - set the PLUGIN_SDK_DIR environment variable")
  endif()
  if(NOT IS_DIRECTORY "${_sdk_unzipped}")
    message(STATUS "Extracting ${_sdk_zip}")
    file(ARCHIVE_EXTRACT INPUT "${_sdk_zip}" DESTINATION "${_sdk_unzipped}")
  endif()
  set(_sdk_search "${_sdk_unzipped}")
endif()
file(TO_CMAKE_PATH "${_sdk_search}" _sdk_search)

set(PLUGIN_SDK_ROOT "")
if(IS_DIRECTORY "${_sdk_search}/plugin_sa")
  set(PLUGIN_SDK_ROOT "${_sdk_search}")
else()
  file(GLOB _sdk_hits LIST_DIRECTORIES true
       "${_sdk_search}/*/plugin_sa" "${_sdk_search}/*/*/plugin_sa")
  list(SORT _sdk_hits)
  foreach(_hit IN LISTS _sdk_hits)
    if(IS_DIRECTORY "${_hit}")
      get_filename_component(PLUGIN_SDK_ROOT "${_hit}" DIRECTORY)
      break()
    endif()
  endforeach()
endif()

if(NOT PLUGIN_SDK_ROOT)
  file(GLOB _sdk_listing RELATIVE "${_sdk_search}" "${_sdk_search}/*")
  message(FATAL_ERROR
    "No 'plugin_sa' folder found under: ${_sdk_search}\n"
    "Top-level entries there: ${_sdk_listing}\n"
    "plugin-sdk.zip must contain the plugin-sdk repository (plugin_sa/, shared/, ...).")
endif()
message(STATUS "plugin-sdk root: ${PLUGIN_SDK_ROOT}")

# -----------------------------------------------------------------------------
#  2. Include directories (only the ones that exist in this SDK version)
# -----------------------------------------------------------------------------
set(PLUGIN_SDK_INCLUDE_DIRS "")
foreach(_rel IN ITEMS plugin_sa plugin_sa/game_sa plugin_sa/game_sa/rw plugin_sa/game_sa/enums
                      shared shared/game)
  if(IS_DIRECTORY "${PLUGIN_SDK_ROOT}/${_rel}")
    list(APPEND PLUGIN_SDK_INCLUDE_DIRS "${PLUGIN_SDK_ROOT}/${_rel}")
  endif()
endforeach()

foreach(_header IN ITEMS plugin.h CPed.h)
  find_path(_found_${_header} NAMES ${_header} PATHS ${PLUGIN_SDK_INCLUDE_DIRS}
            NO_DEFAULT_PATH NO_CACHE)
  if(NOT _found_${_header})
    message(FATAL_ERROR
      "'${_header}' not found in the plugin-sdk include folders:\n  ${PLUGIN_SDK_INCLUDE_DIRS}\n"
      "plugin-sdk.zip does not look like a complete plugin-sdk checkout.")
  endif()
endforeach()

# -----------------------------------------------------------------------------
#  3. plugin-sdk library: prebuilt plugin.lib or compiled from source
# -----------------------------------------------------------------------------
file(GLOB_RECURSE _lib_release "${PLUGIN_SDK_ROOT}/plugin.lib")
file(GLOB_RECURSE _lib_debug   "${PLUGIN_SDK_ROOT}/plugin_d.lib")
list(SORT _lib_release)
list(SORT _lib_debug)

string(TOUPPER "${PLUGIN_SDK_BUILD_FROM_SOURCE}" _from_source)
set(_use_prebuilt FALSE)
if(_from_source STREQUAL "OFF")
  if(NOT _lib_release)
    message(FATAL_ERROR "PLUGIN_SDK_BUILD_FROM_SOURCE=OFF but no plugin.lib exists under ${PLUGIN_SDK_ROOT}")
  endif()
  set(_use_prebuilt TRUE)
elseif(_from_source STREQUAL "AUTO" AND _lib_release)
  set(_use_prebuilt TRUE)
endif()

if(_use_prebuilt)
  list(GET _lib_release 0 _lib_release_path)
  add_library(plugin_sdk_sa STATIC IMPORTED)
  set_target_properties(plugin_sdk_sa PROPERTIES IMPORTED_LOCATION "${_lib_release_path}")
  message(STATUS "plugin-sdk: using prebuilt ${_lib_release_path}")
  if(_lib_debug)
    list(GET _lib_debug 0 _lib_debug_path)
    set_target_properties(plugin_sdk_sa PROPERTIES IMPORTED_LOCATION_DEBUG "${_lib_debug_path}")
    message(STATUS "plugin-sdk: Debug uses ${_lib_debug_path}")
  endif()
  target_include_directories(plugin_sdk_sa INTERFACE ${PLUGIN_SDK_INCLUDE_DIRS})
  target_compile_definitions(plugin_sdk_sa INTERFACE
      GTASA ${PLUGIN_SDK_GAME_VERSION}
      _CRT_SECURE_NO_WARNINGS _CRT_NON_CONFORMING_SWPRINTFS _USE_MATH_DEFINES)
else()
  file(GLOB_RECURSE _sdk_sources CONFIGURE_DEPENDS
       "${PLUGIN_SDK_ROOT}/plugin_sa/*.cpp"
       "${PLUGIN_SDK_ROOT}/shared/*.cpp")
  list(FILTER _sdk_sources EXCLUDE REGEX "/(examples?|tools|docs?|tests?)/")
  if(NOT _sdk_sources)
    message(FATAL_ERROR "No plugin-sdk .cpp sources found under ${PLUGIN_SDK_ROOT}/plugin_sa and /shared")
  endif()
  list(LENGTH _sdk_sources _sdk_source_count)
  message(STATUS "plugin-sdk: no prebuilt plugin.lib, compiling ${_sdk_source_count} SDK sources")

  add_library(plugin_sdk_sa STATIC ${_sdk_sources})
  target_include_directories(plugin_sdk_sa PUBLIC ${PLUGIN_SDK_INCLUDE_DIRS})
  target_compile_definitions(plugin_sdk_sa PUBLIC
      GTASA ${PLUGIN_SDK_GAME_VERSION}
      _CRT_SECURE_NO_WARNINGS _CRT_NON_CONFORMING_SWPRINTFS _USE_MATH_DEFINES)
  if(MSVC)
    target_compile_options(plugin_sdk_sa PRIVATE /MP /w)   # third-party code: no warning noise
  endif()
endif()

# -----------------------------------------------------------------------------
#  4. The plugin itself -> MinecraftSA.asi
# -----------------------------------------------------------------------------
add_library(MinecraftSA SHARED
    Minecraftsa.cpp
    Elytraphysics.h)

target_link_libraries(MinecraftSA PRIVATE plugin_sdk_sa)

set_target_properties(MinecraftSA PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "MinecraftSA"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist/$<CONFIG>")

if(MSVC)
  target_compile_options(MinecraftSA PRIVATE /MP /W3)
endif()

# Copy any .ini from the repo root next to the .asi (the plugin reads MinecraftSA.ini).
file(GLOB _ini_files "${CMAKE_CURRENT_SOURCE_DIR}/*.ini")
foreach(_ini IN LISTS _ini_files)
  add_custom_command(TARGET MinecraftSA POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ini}" "$<TARGET_FILE_DIR:MinecraftSA>")
endforeach()
