# FindLibObs.cmake — locate the libobs SDK without building OBS itself.
#
# This project links a plugin MODULE against libobs whose symbols are resolved
# at runtime by the OBS process.  We therefore only need:
#   * the libobs *headers*  (LIBOBS_INCLUDE_DIR)
#   * something to satisfy the linker:
#       - Windows : an import library generated from the installed obs.dll
#                   (OBS_IMPORT_LIB), see scripts/package-plugin.ps1
#       - Linux   : a stub shared library with soname libobs.so.0
#                   (OBS_STUB_LIB), see scripts/package-plugin.sh
#       - macOS   : nothing (-undefined dynamic_lookup handles it)
#
# Usage:
#   list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
#   find_package(LibObs REQUIRED)
#   target_link_libraries(<target> PRIVATE LibObs::LibObs)
#
# All variables are honoured if provided (packaging scripts set them), with
# best-effort auto-detection as a convenience for manual builds.

set(LibObs_FOUND FALSE)

# --- 1. Headers ----------------------------------------------------------
if(NOT LIBOBS_INCLUDE_DIR)
  # Common locations where a shallow obs-studio clone may live
  foreach(candidate
      "${CMAKE_CURRENT_SOURCE_DIR}/build/plugin-sdk/obs-studio/libobs"
      "${CMAKE_CURRENT_SOURCE_DIR}/../obs-studio/libobs")
    if(EXISTS "${candidate}/obs-module.h")
      set(LIBOBS_INCLUDE_DIR "${candidate}")
      break()
    endif()
  endforeach()
endif()

if(NOT LIBOBS_INCLUDE_DIR OR NOT EXISTS "${LIBOBS_INCLUDE_DIR}/obs-module.h")
  message(FATAL_ERROR
    "libobs headers not found. Set -DLIBOBS_INCLUDE_DIR=<obs-studio>/libobs "
    "(headers only, no OBS build required).")
endif()

# --- 2. Linker input ------------------------------------------------------
if(WIN32 AND NOT OBS_IMPORT_LIB)
  message(FATAL_ERROR
    "OBS_IMPORT_LIB not set. Generate obs.lib from obs.dll with "
    "dumpbin+lib (see scripts/package-plugin.ps1).")
endif()
if(UNIX AND NOT APPLE AND NOT OBS_STUB_LIB)
  message(FATAL_ERROR
    "OBS_STUB_LIB not set. Create a stub libobs.so with soname libobs.so.0 "
    "(see scripts/package-plugin.sh).")
endif()

# Interface target carrying the headers only; platform linker inputs are added
# by the top-level CMakeLists (obs.lib / libobs stub / dynamic_lookup).
if(NOT TARGET LibObs::LibObs)
  add_library(LibObs::LibObs INTERFACE IMPORTED GLOBAL)
  set_target_properties(LibObs::LibObs PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LIBOBS_INCLUDE_DIR}")
endif()

set(LibObs_FOUND TRUE)
