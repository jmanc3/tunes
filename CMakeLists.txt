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

include(CTest)
if(BUILD_TESTING)
    add_executable(tunes_pipewire_settings_test tests/pipewire_settings_test.cpp src/pipewire_settings.cpp)
    target_include_directories(tunes_pipewire_settings_test PRIVATE include)
    target_link_libraries(tunes_pipewire_settings_test PRIVATE PkgConfig::DEPS)
    add_test(NAME pipewire_settings COMMAND tunes_pipewire_settings_test)
    set_tests_properties(pipewire_settings PROPERTIES TIMEOUT 10)
    add_executable(tunes_album_art_test tests/album_art_test.cpp src/album_art.cpp)
    target_include_directories(tunes_album_art_test PRIVATE include)
    target_link_libraries(tunes_album_art_test PRIVATE PkgConfig::DEPS)
    add_test(NAME album_art_cache COMMAND tunes_album_art_test)
    add_executable(tunes_session_test tests/session_state_test.cpp src/session_state.cpp src/audio_data.cpp)
    target_include_directories(tunes_session_test PRIVATE include)
    target_link_libraries(tunes_session_test PRIVATE PkgConfig::DEPS)
    add_test(NAME session_state COMMAND tunes_session_test)
    add_executable(tunes_player_session_test tests/player_session_test.cpp src/player.cpp src/miniaudio.c)
    target_include_directories(tunes_player_session_test PRIVATE include)
    target_link_libraries(tunes_player_session_test PRIVATE PkgConfig::DEPS)
    target_link_options(tunes_player_session_test PRIVATE
        -Wl,--wrap=ma_device_init -Wl,--wrap=ma_device_uninit
        -Wl,--wrap=ma_device_start -Wl,--wrap=ma_device_stop)
    add_test(NAME paused_session_restore COMMAND tunes_player_session_test)
    add_executable(tunes_sample_rate_test tests/player_sample_rate_test.cpp src/player.cpp src/miniaudio.c)
    target_include_directories(tunes_sample_rate_test PRIVATE include)
    target_link_libraries(tunes_sample_rate_test PRIVATE PkgConfig::DEPS)
    target_link_options(tunes_sample_rate_test PRIVATE -Wl,--wrap=ma_device_init -Wl,--wrap=ma_device_start)
    add_test(NAME sample_rate_switching COMMAND tunes_sample_rate_test)
    set_tests_properties(sample_rate_switching PROPERTIES TIMEOUT 20)
endif()

install(TARGETS tunes
        DESTINATION /usr/local/bin/)
