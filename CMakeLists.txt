cmake_minimum_required(VERSION 4.2.2)

project(tunes LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    src/*.c
    src/*.cpp
)

add_executable(tunes ${SOURCES})

target_include_directories(tunes PRIVATE include)

find_package(PkgConfig REQUIRED)

pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
    cairo
    pango
    pangocairo
    librsvg-2.0
    wayland-server
    wayland-client
    wayland-cursor
    xkbcommon
    libdrm
    pixman-1
    hyprland
    libinput
    libudev
    gio-2.0
    dbus-1
    alsa
    libpipewire-0.3
    libpulse
    gtk+-3.0
    libmagic
    taglib
)

target_link_libraries(tunes PRIVATE PkgConfig::DEPS)

install(TARGETS tunes
        DESTINATION /usr/local/bin/)
