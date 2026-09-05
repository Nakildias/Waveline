# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
cmake_minimum_required(VERSION 3.21)
project(waveline VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ------------------------------------------------------------- profile lib
# Which microphone this was installed for. Just a config file reader, so the
# GUI can link it without linking the USB transport -- see the note on the GUI
# target below, which that separation exists to keep true.
add_library(wavelineprofile STATIC
    src/device/deviceprofile.cpp
)
target_include_directories(wavelineprofile PUBLIC src)

# ---------------------------------------------------------------- device lib
# Deliberately free of Qt and PipeWire: the daemon links it and it is the one
# place the USB safety interlocks live. One .cpp per microphone that has a
# vendor protocol -- adding a device means adding a file here and a directory
# under devices/.
add_library(wavelinedevice STATIC
    src/device/wave3device.cpp
)
target_include_directories(wavelinedevice PUBLIC src)
target_link_libraries(wavelinedevice PUBLIC wavelineprofile)

# ------------------------------------------------------------------- probe
# Headless check that the device layer works, with no Qt involved. Useful on
# its own when the GUI will not start.
add_executable(waveline-probe src/tools/probe_main.cpp)
target_link_libraries(waveline-probe PRIVATE wavelinedevice)

# ------------------------------------------------------------------ engine
# The PipeWire graph: channels, dual mixes, routable software monitor. Also
# Qt-free, so the daemon can use it without dragging in a GUI toolkit.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

pkg_check_modules(RNNOISE REQUIRED IMPORTED_TARGET rnnoise)

# DeepFilterNet is the second noise suppression engine. It is deliberately NOT
# a build dependency: it ships no pkg-config file, is packaged under a
# different name everywhere it is packaged at all, and requiring it would mean
# needing a Rust toolchain to build this. src/engine/denoiser.cpp dlopen()s
# libdf.so at runtime instead, so all that is needed here is libdl.

add_library(wavelineengine STATIC
    src/engine/filterhost.cpp
    src/engine/pwengine.cpp
    src/engine/mixergraph.cpp
    src/engine/denoiser.cpp
    src/engine/noisefilter.cpp
    src/engine/effectsfilter.cpp
    src/engine/dynamics.cpp
    src/engine/dynamicsfilter.cpp
    src/engine/ducking.cpp
    src/engine/duckingfilter.cpp
    src/engine/lufslimiter.cpp
    src/engine/lufslimiterfilter.cpp
    src/engine/creativefx.cpp
    src/engine/creativefxfilter.cpp
    src/engine/fluidsynthbackend.cpp
    src/engine/fluidsynthfilter.cpp
    src/engine/gainfilter.cpp
    src/engine/channelinputmixer.cpp
    src/engine/captureselector.cpp
    src/engine/approuter.cpp
    src/engine/steamdetector.cpp
    src/engine/appidentity.cpp
    src/engine/soundboardengine.cpp
    src/engine/soundsharerouter.cpp
    src/engine/levelprobe.cpp
    src/engine/alsadelay.cpp
    src/engine/dspprobe.cpp
    src/engine/rtsched.cpp
    src/engine/tuner.cpp
)
target_include_directories(wavelineengine PUBLIC src)
target_link_libraries(wavelineengine PUBLIC PkgConfig::PIPEWIRE PkgConfig::RNNOISE
                                         ${CMAKE_DL_LIBS})

set(WAVELINE_FLUIDSYNTH_LIB "${CMAKE_CURRENT_SOURCE_DIR}/lib/libfluidsynth.so.3")
if(EXISTS "${WAVELINE_FLUIDSYNTH_LIB}")
    target_compile_definitions(wavelineengine PRIVATE WAVELINE_FLUIDSYNTH_LINKED=1)
endif()

add_executable(waveline-graph src/tools/graph_main.cpp)
target_link_libraries(waveline-graph PRIVATE wavelineengine)

add_executable(waveline-nc src/tools/nc_main.cpp)
target_link_libraries(waveline-nc PRIVATE wavelineengine)

# --------------------------------------------------------------------- gui
# Svg is needed for the icons: they are recoloured at load time by substituting
# `currentColor`, then rendered with QSvgRenderer.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets DBus Svg Network WebSockets)
qt_standard_project_setup()

set(WAVELINE_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
# file(READ) happens at configure time and creates no build-time dependency, so
# without this an edited VERSION would not reach version.h until something else
# forced a reconfigure -- and the binary would keep reporting the old release.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WAVELINE_VERSION_FILE}")
if(EXISTS "${WAVELINE_VERSION_FILE}")
    file(READ "${WAVELINE_VERSION_FILE}" WAVELINE_VERSION_STRING)
    string(STRIP "${WAVELINE_VERSION_STRING}" WAVELINE_VERSION_STRING)
else()
    set(WAVELINE_VERSION_STRING "unknown")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
    @ONLY
)

# ------------------------------------------------------------------ daemon
# QCoreApplication only: wavelined must run without a display, from a user unit.
qt_add_executable(wavelined
    src/daemon/main.cpp
    src/daemon/mixerservice.cpp
    src/daemon/configstore.cpp
    src/daemon/alsaaliases.cpp
    src/daemon/wpheadroom.cpp
    src/daemon/companionserver.cpp
    # Device names a desktop shell renamed, when one has -- optional, one-way,
    # and absent on a machine with no such shell. See desktopnames.h.
    src/daemon/desktopnames.cpp
    src/daemon/configstore.h
    src/daemon/mixerservice.h
    src/daemon/alsaaliases.h
    src/daemon/wpheadroom.h
    src/daemon/companionserver.h
    src/daemon/desktopnames.h
)
# The version string the companion page shows in its header comes from the same
# generated header the GUI uses, so the two can never disagree about what is
# running.
target_include_directories(wavelined PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(wavelined PRIVATE
    wavelineengine wavelinedevice Qt6::Core Qt6::DBus Qt6::Network
    Qt6::WebSockets)

# The companion page is compiled into the daemon rather than installed beside
# it. A daemon that serves files from a data directory is a daemon that serves
# nothing at all when the directory is missing -- and there is no ordinary way
# for a user to notice that has happened until a tablet shows a blank page.
qt_add_resources(wavelined "waveline-companion"
    PREFIX "/companion"
    BASE src/daemon/web
    FILES
        src/daemon/web/index.html
        src/daemon/web/app.css
        src/daemon/web/app.js
        src/daemon/web/icon.svg
        # iOS will not take an SVG for a home-screen icon, so the PNG the
        # desktop application uses is served beside it, pre-squared: iOS lays
        # its own rounded mask over whatever it is given.
        src/daemon/web/app-icon.png
        src/daemon/web/manifest.webmanifest
)

if(EXISTS "${WAVELINE_FLUIDSYNTH_LIB}")
    target_link_libraries(wavelined PRIVATE "${WAVELINE_FLUIDSYNTH_LIB}")
    set_target_properties(wavelined PROPERTIES
        BUILD_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
endif()

# -------------------------------------------------------------------- cli
# A pure D-Bus client, same as the GUI: no PipeWire, no USB, just
# org.waveline.Mixer calls. This is the thing a Stream Deck or a keybind
# actually runs -- a single-purpose flag per invocation, so one button maps
# to one command line with nothing to script around it.
add_executable(wavelined-cli src/tools/cli_main.cpp)
target_include_directories(wavelined-cli PRIVATE src)
target_link_libraries(wavelined-cli PRIVATE Qt6::Core Qt6::DBus)

# The GUI is a pure D-Bus client: it links neither the USB device layer nor
# PipeWire, so it cannot fight the daemon for the vendor interface. The profile
# library is the one exception and reads nothing but a config file -- the GUI
# needs the answer before the daemon is up, to know whether to show a hardware
# panel at all.
qt_add_executable(waveline-mixer
    src/main.cpp
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
    src/ui/theme.cpp
    src/ui/theme.h
    src/ui/widgets.cpp
    src/ui/widgets.h
    src/ui/levelmeter.cpp
    src/ui/levelmeter.h
    src/ui/channelstrip.cpp
    src/ui/channelstrip.h
    src/ui/cardidentity.cpp
    src/ui/cardidentity.h
    src/ui/mixerclient.cpp
    src/ui/mixerclient.h
    src/ui/profileswindow.cpp
    src/ui/profileswindow.h
    src/ui/appstab.cpp
    src/ui/appstab.h
    src/ui/soundboardwindow.cpp
    src/ui/soundboardwindow.h
    src/ui/soundtrimdialog.cpp
    src/ui/soundtrimdialog.h
    src/ui/soundsharingtab.cpp
    src/ui/soundsharingtab.h
    src/ui/effectswindow.cpp
    src/ui/effectswindow.h
    src/ui/creativefxpanel.cpp
    src/ui/creativefxpanel.h
    src/ui/creativerackwindow.cpp
    src/ui/creativerackwindow.h
    src/ui/rackpresetstore.cpp
    src/ui/rackpresetstore.h
    src/ui/proeqwindow.cpp
    src/ui/proeqwindow.h
    src/ui/aboutwindow.cpp
    src/ui/settingswindow.cpp
    src/ui/aboutwindow.h
    src/ui/settingswindow.h
    src/ui/tunerdata.cpp
    src/ui/tunerdata.h
    src/ui/tunerwindow.cpp
    src/ui/tunerwindow.h
    src/ui/companionwindow.cpp
    src/ui/companionwindow.h
)
target_include_directories(waveline-mixer PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Compiled in rather than installed to a data directory: the mixer is a single
# binary users copy into ~/.local/bin, and an icon set that can go missing is a
# support problem for no benefit.
# A prefix of our own, not "/icons": Qt style and theme plugins register
# their own icons at :/icons, and listing that directory for the icon
# picker turned up glyphs from whatever else happened to be loaded.
qt_add_resources(waveline-mixer "waveline-icons"
    PREFIX "/waveline"
    BASE src/icons
    FILES
        src/icons/browser.svg
        src/icons/arrow-down.svg
        src/icons/arrow-left.svg
        src/icons/arrow-right.svg
        src/icons/arrow-up.svg
        src/icons/build.svg
        src/icons/edit.svg
        src/icons/export.svg
        src/icons/import.svg
        src/icons/save.svg
        src/icons/ear-off.svg
        src/icons/ear.svg
        src/icons/equal.svg
        src/icons/game.svg
        src/icons/gear.svg
        src/icons/headphones-off.svg
        src/icons/headphones.svg
        src/icons/heartbeat.svg
        src/icons/info.svg
        src/icons/microphone-off.svg
        src/icons/microphone.svg
        src/icons/minus.svg
        src/icons/music.svg
        src/icons/plus.svg
        src/icons/speaker-high.svg
        src/icons/speaker-low.svg
        src/icons/speaker-muted.svg
        src/icons/stream-off.svg
        src/icons/stop.svg
        src/icons/stream.svg
        src/icons/switch-profile.svg
        src/icons/system.svg
        src/icons/sfx.svg
        src/icons/trash.svg
        src/icons/x.svg
        src/icons/video.svg
        src/icons/voice.svg
        src/icons/camera.svg
        src/icons/chevron.svg
        src/icons/capture.svg
        src/icons/game-alt.svg
        src/icons/phone.svg
        src/icons/piano.svg
        src/icons/play.svg
        src/icons/speaker.svg
        src/icons/webcam.svg
        src/icons/webcam-alt.svg
        src/icons/waveline-mixer.png
)
target_link_libraries(waveline-mixer PRIVATE
    wavelineprofile Qt6::Core Qt6::Widgets Qt6::DBus Qt6::Svg Qt6::Network)

install(TARGETS wavelined waveline-mixer wavelined-cli waveline-probe waveline-graph waveline-nc
        RUNTIME DESTINATION bin)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/libfluidsynth.so.3")
    install(FILES lib/libfluidsynth.so.3 DESTINATION lib)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/libfluidsynth.so")
    install(FILES lib/libfluidsynth.so DESTINATION lib)
endif()
