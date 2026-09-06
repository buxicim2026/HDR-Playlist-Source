cmake_minimum_required(VERSION 3.16...3.28)

project(HDR-Playlist-Source VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# OBS SDK.  Only the libobs *headers* plus a linker input are needed; real
# symbols resolve at runtime against the OBS process.
#
#   -DLIBOBS_INCLUDE_DIR=<obs-studio checkout>/libobs
#
#   Windows additionally wants an import library generated from the installed
#   obs.dll (see scripts/package-plugin.ps1):
#     -DOBS_IMPORT_LIB=<path>/obs.lib
#
#   Linux links a stub shared library with soname libobs.so.0:
#     -DOBS_STUB_LIB=<path>/libobs.so
#
#   macOS uses -undefined dynamic_lookup and needs nothing extra.
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(LibObs REQUIRED)

set(HDRP_SOURCES
  src/plugin-main.c
  src/playlist.c
  src/util.c
  src/switcher.c
  src/audio.c
  src/hdr_playlist_source.c)

set(HDRP_MODULE_NAME "HDR-Playlist-Source")
configure_file(
  src/plugin-support.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/plugin-support.h @ONLY)

add_library(hdr-playlist-source MODULE ${HDRP_SOURCES})
target_include_directories(hdr-playlist-source PRIVATE
  "${LIBOBS_INCLUDE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(hdr-playlist-source PRIVATE LibObs::LibObs)

set_target_properties(hdr-playlist-source PROPERTIES
  PREFIX ""
  OUTPUT_NAME "HDR-Playlist-Source")

if(WIN32)
  # No extra system libs: threading uses CRITICAL_SECTION, no sockets.
  if(OBS_IMPORT_LIB)
    target_link_libraries(hdr-playlist-source PRIVATE "${OBS_IMPORT_LIB}")
  else()
    message(FATAL_ERROR
      "OBS_IMPORT_LIB is not set. Generate obs.lib from obs.dll with "
      "dumpbin+lib (see scripts/package-plugin.ps1).")
  endif()
elseif(APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(hdr-playlist-source PRIVATE Threads::Threads)
  target_link_options(hdr-playlist-source PRIVATE "-undefined" "dynamic_lookup")
  set_target_properties(hdr-playlist-source PROPERTIES SUFFIX "")
else()
  find_package(Threads REQUIRED)
  target_link_libraries(hdr-playlist-source PRIVATE Threads::Threads)
  if(OBS_STUB_LIB)
    target_link_libraries(hdr-playlist-source PRIVATE "${OBS_STUB_LIB}")
  else()
    message(FATAL_ERROR
      "OBS_STUB_LIB is not set. Create a stub libobs.so with soname "
      "libobs.so.0 (see scripts/package-plugin.sh).")
  endif()
endif()

if(MSVC)
  target_compile_options(hdr-playlist-source PRIVATE /W3)
else()
  target_compile_options(hdr-playlist-source PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
