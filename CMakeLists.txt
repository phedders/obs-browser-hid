cmake_minimum_required(VERSION 3.16)
project(obs-browser-hid LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# obs-browser-hid
#
# Standalone OBS plugin adding the obs-websocket vendor requests `SendKey`,
# `SendText` and `SendMouseClick` to send keyboard/mouse input to a named
# browser source over websockets.
#
# Build against the local obs-studio checkout:
#   cmake -B build -S . -DOBS_STUDIO_DIR=/home/prh/gits/obs-studio
#   cmake --build build
#
# Install the module where OBS loads plugins from
# (default here matches the self-built deb layout):
#   cmake --install build --prefix /            # -> /usr/lib/x86_64-linux-gnu/obs-modules/core/
# or copy build/obs-browser-hid.so manually, e.g. into
# /home/prh/gits/obs-studio/build/rundir/RelWithDebInfo/lib/x86_64-linux-gnu/obs-modules/core/
# ---------------------------------------------------------------------------

set(OBS_STUDIO_DIR "/home/prh/gits/obs-studio" CACHE PATH "obs-studio source tree")
set(OBS_BUILD_DIR "${OBS_STUDIO_DIR}/build" CACHE PATH
    "obs-studio build tree containing the libobs build outputs")

set(OBS_PLUGIN_INSTALL_DIR
    "/usr/lib/${CMAKE_SYSTEM_PROCESSOR}/obs-modules/core" CACHE PATH
    "Where the plugin module is installed (matches your self-built deb layout)")

find_package(PkgConfig REQUIRED)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(obs-browser-hid MODULE
	obs-browser-hid.c
	obs-browser-hid-interact.cpp
	obs-browser-hid-ui.cpp)

target_include_directories(obs-browser-hid PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${OBS_BUILD_DIR}/config
	${OBS_STUDIO_DIR}/libobs
	${OBS_STUDIO_DIR}/frontend/api)

target_link_libraries(obs-browser-hid PRIVATE
	${OBS_BUILD_DIR}/libobs/libobs.so
	${OBS_BUILD_DIR}/frontend/api/libobs-frontend-api.so
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

install(TARGETS obs-browser-hid
	LIBRARY DESTINATION ${OBS_PLUGIN_INSTALL_DIR})
