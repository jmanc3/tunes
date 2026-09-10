cmake_minimum_required(VERSION 4.2.2)

project(tunes LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    src/*.c
    src/*.cpp
)

file(GLOB DRAWING_SOURCES CONFIGURE_DEPENDS src/drawing/*.cpp)
list(REMOVE_ITEM SOURCES ${DRAWING_SOURCES})
add_library(tunes_drawing STATIC ${DRAWING_SOURCES})
target_include_directories(tunes_drawing PUBLIC include)

add_executable(tunes ${SOURCES})

target_include_directories(tunes PRIVATE include)

find_package(PkgConfig REQUIRED)

pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
    cairo
    egl
    glesv2
    wayland-egl
    pango
    pangocairo
    librsvg-2.0
    wayland-server
    wayland-client
    wayland-cursor
    xkbcommon
    libdrm
    pixman-1
    libinput
    libudev
    gio-2.0
    dbus-1
    alsa
    gtk+-3.0
    libpipewire-0.3
    libpulse
    libmagic
    taglib
)

target_link_libraries(tunes_drawing PRIVATE PkgConfig::DEPS)
target_link_libraries(tunes PRIVATE tunes_drawing PkgConfig::DEPS)

include(CTest)
if(BUILD_TESTING)
    add_executable(tunes_library_scrollbar_test tests/library_scrollbar_test.cpp)
    target_include_directories(tunes_library_scrollbar_test PRIVATE include)
    add_test(NAME library_scrollbar COMMAND tunes_library_scrollbar_test)
    add_executable(tunes_egl_window_test tests/egl_window_test.cpp src/client/egl_window.cpp)
    target_include_directories(tunes_egl_window_test PRIVATE include)
    target_link_libraries(tunes_egl_window_test PRIVATE tunes_drawing PkgConfig::DEPS)
    add_test(NAME egl_window COMMAND tunes_egl_window_test)
    set_tests_properties(egl_window PROPERTIES TIMEOUT 30 ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1")
    add_executable(tunes_opengl_drawing_test tests/opengl_drawing_test.cpp)
    target_include_directories(tunes_opengl_drawing_test PRIVATE include)
    target_link_libraries(tunes_opengl_drawing_test PRIVATE tunes_drawing PkgConfig::DEPS)
    add_test(NAME opengl_drawing COMMAND tunes_opengl_drawing_test)
    set_tests_properties(opengl_drawing PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_executable(tunes_drawing_test tests/drawing_test.cpp)
    target_include_directories(tunes_drawing_test PRIVATE include)
    target_link_libraries(tunes_drawing_test PRIVATE tunes_drawing PkgConfig::DEPS)
    add_test(NAME drawing COMMAND tunes_drawing_test)
    add_executable(tunes_popup_overlay_test tests/popup_overlay_test.cpp src/events.cpp src/container.cpp)
    target_include_directories(tunes_popup_overlay_test PRIVATE include)
    target_link_libraries(tunes_popup_overlay_test PRIVATE PkgConfig::DEPS)
    add_test(NAME popup_overlay COMMAND tunes_popup_overlay_test)
    add_executable(tunes_scroll_hover_test tests/scroll_hover_test.cpp src/events.cpp src/container.cpp)
    target_include_directories(tunes_scroll_hover_test PRIVATE include)
    target_link_libraries(tunes_scroll_hover_test PRIVATE PkgConfig::DEPS)
    add_test(NAME scroll_hover COMMAND tunes_scroll_hover_test)
    add_executable(tunes_playback_queue_test tests/playback_queue_test.cpp)
    target_include_directories(tunes_playback_queue_test PRIVATE include)
    add_test(NAME playback_queue COMMAND tunes_playback_queue_test)
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
    add_executable(tunes_session_settings_test tests/session_settings_test.cpp src/session_state.cpp src/audio_data.cpp)
    target_include_directories(tunes_session_settings_test PRIVATE include)
    target_link_libraries(tunes_session_settings_test PRIVATE PkgConfig::DEPS)
    add_test(NAME session_settings COMMAND tunes_session_settings_test)
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
