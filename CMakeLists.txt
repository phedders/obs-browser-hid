cmake_minimum_required(VERSION 3.16)
project(obs-browser-hid VERSION 1.0.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# obs-browser-hid
#
# Standalone OBS plugin adding the obs-websocket vendor requests `SendKey`,
# `SendText` and `SendMouseClick` to send keyboard/mouse input to a named
# browser source over websockets, plus the "Browser HID" dock.
#
# Developer build against a local obs-studio checkout:
#   cmake -B build -S . -DOBS_STUDIO_DIR=/path/to/obs-studio
#   cmake --build build
#
# Package/distro build against the installed OBS 33 headers/libraries:
#   cmake -B build -S . -DOBS_USE_SYSTEM_LIBS=ON
#   cmake --build build
#
# OBS 33 loads third-party (non-core) modules from:
#   <libdir>/obs-modules/plugins/<module>.so
# with per-module data under:
#   <datadir>/obs/obs-modules/plugins/<module>/
# (libdir is e.g. lib/x86_64-linux-gnu on Debian or lib64 on Fedora).
# ---------------------------------------------------------------------------

include(GNUInstallDirs)

option(OBS_USE_SYSTEM_LIBS
    "Build against installed libobs/obs-frontend-api headers and libraries"
    OFF)

if(OBS_USE_SYSTEM_LIBS)
    find_path(OBS_INCLUDE_DIR obs.h PATH_SUFFIXES obs)
    find_library(OBS_LIBRARY NAMES obs)
    find_path(OBS_FRONTEND_INCLUDE_DIR obs-frontend-api.h PATH_SUFFIXES obs)
    find_library(OBS_FRONTEND_LIBRARY NAMES obs-frontend-api)

    if(NOT OBS_INCLUDE_DIR OR NOT OBS_LIBRARY OR
       NOT OBS_FRONTEND_INCLUDE_DIR OR NOT OBS_FRONTEND_LIBRARY)
        message(FATAL_ERROR
            "libobs/obs-frontend-api development files were not found. "
            "Install OBS 33 with its headers, or configure with "
            "-DOBS_USE_SYSTEM_LIBS=OFF -DOBS_STUDIO_DIR=/path/to/obs-studio")
    endif()

    set(OBS_INCLUDE_DIRS ${OBS_INCLUDE_DIR} ${OBS_FRONTEND_INCLUDE_DIR})
    set(OBS_LIBRARIES ${OBS_FRONTEND_LIBRARY} ${OBS_LIBRARY})
else()
    set(OBS_STUDIO_DIR "/home/prh/gits/obs-studio" CACHE PATH "obs-studio source tree")
    set(OBS_BUILD_DIR "${OBS_STUDIO_DIR}/build" CACHE PATH
        "obs-studio build tree containing the libobs build outputs")

    if(NOT EXISTS "${OBS_BUILD_DIR}/libobs/libobs.so")
        message(FATAL_ERROR
            "libobs was not found under ${OBS_BUILD_DIR}. Build obs-studio "
            "first, or configure with -DOBS_USE_SYSTEM_LIBS=ON")
    endif()

    set(OBS_INCLUDE_DIRS
        ${OBS_BUILD_DIR}/config
        ${OBS_STUDIO_DIR}/libobs
        ${OBS_STUDIO_DIR}/frontend/api)
    set(OBS_LIBRARIES
        ${OBS_BUILD_DIR}/frontend/api/libobs-frontend-api.so
        ${OBS_BUILD_DIR}/libobs/libobs.so)
endif()

set(OBS_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/obs-modules/plugins"
    CACHE STRING
    "Third-party plugin module install destination (relative to the prefix)")
set(OBS_PLUGIN_DATA_INSTALL_DIR
    "${CMAKE_INSTALL_DATADIR}/obs/obs-modules/plugins/${PROJECT_NAME}"
    CACHE STRING
    "Third-party plugin data install destination (relative to the prefix)")

find_package(PkgConfig REQUIRED)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(obs-browser-hid MODULE
	obs-browser-hid.c
	obs-browser-hid-interact.cpp
	obs-browser-hid-ui.cpp)

target_include_directories(obs-browser-hid PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${OBS_INCLUDE_DIRS})

target_link_libraries(obs-browser-hid PRIVATE
	${OBS_LIBRARIES}
	Qt6::Widgets
	PkgConfig::XKBCOMMON)

set_target_properties(obs-browser-hid PROPERTIES
	PREFIX ""
	C_STANDARD 11
	C_STANDARD_REQUIRED YES
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED YES)
target_compile_definitions(obs-browser-hid PRIVATE QT_NO_KEYWORDS)
target_compile_options(obs-browser-hid PRIVATE -Wall -Wextra)

configure_file(manifest.json.in manifest.json @ONLY)

install(TARGETS obs-browser-hid
	LIBRARY DESTINATION ${OBS_PLUGIN_INSTALL_DIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/manifest.json
	DESTINATION ${OBS_PLUGIN_DATA_INSTALL_DIR})
