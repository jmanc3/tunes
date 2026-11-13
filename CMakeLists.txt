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

# --- Visualizer: projectM (libprojectM 4, OpenGL ES build) -------------------
# Sources are fetched once into .deps/ in the source tree and shared by every
# build directory. Point TUNES_PROJECTM_SOURCE_DIR at an existing checkout to
# build offline.
include(FetchContent)
set(TUNES_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
set(TUNES_PROJECTM_SOURCE_DIR "" CACHE PATH "Existing projectM checkout (skips download)")
option(TUNES_FETCH_PRESETS "Download the projectM preset and texture packs for the visualizer" ON)

set(ENABLE_GLES ON CACHE BOOL "" FORCE)
set(ENABLE_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ENABLE_PLAYLIST OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL_UI OFF CACHE BOOL "" FORCE)
set(ENABLE_SYSTEM_PROJECTM_EVAL OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(TUNES_PROJECTM_SOURCE_DIR)
    set(FETCHCONTENT_SOURCE_DIR_PROJECTM "${TUNES_PROJECTM_SOURCE_DIR}")
endif()
FetchContent_Declare(projectm
    GIT_REPOSITORY https://github.com/projectM-visualizer/projectm.git
    GIT_TAG eae22de40144ee8ded92f3c04b93a4c2f82adcc0
    GIT_SUBMODULES vendor/projectm-eval
    GIT_SHALLOW FALSE
    SOURCE_DIR "${TUNES_DEPS_DIR}/projectm"
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(projectm)
target_link_libraries(tunes PRIVATE libprojectM::projectM)

set(TUNES_PRESETS_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/share/tunes")
target_compile_definitions(tunes PRIVATE
    TUNES_VISUALIZER_INSTALL_DIR="${TUNES_PRESETS_INSTALL_DIR}"
    TUNES_VISUALIZER_SOURCE_DIR="${TUNES_DEPS_DIR}")
if(TUNES_FETCH_PRESETS)
    FetchContent_Declare(tunes_presets
        GIT_REPOSITORY https://github.com/projectM-visualizer/presets-cream-of-the-crop.git
        GIT_TAG 0180df21f5e0bd39b9060cc5de420ed2f1f9e509
        SOURCE_DIR "${TUNES_DEPS_DIR}/presets"
    )
    FetchContent_Declare(tunes_textures
        GIT_REPOSITORY https://github.com/projectM-visualizer/presets-milkdrop-texture-pack.git
        GIT_TAG 6368812f27bc747b517218fbf89d21d59afce4d9
        SOURCE_DIR "${TUNES_DEPS_DIR}/textures"
    )
    FetchContent_MakeAvailable(tunes_presets tunes_textures)
    install(DIRECTORY "${TUNES_DEPS_DIR}/presets/" DESTINATION "${TUNES_PRESETS_INSTALL_DIR}/presets"
            PATTERN ".git" EXCLUDE)
    install(DIRECTORY "${TUNES_DEPS_DIR}/textures/textures/" DESTINATION "${TUNES_PRESETS_INSTALL_DIR}/textures")
endif()

# include(CTest)
# if(BUILD_TESTING)
#     add_executable(tunes_conversion_test tests/audio_conversion_test.cpp src/audio_conversion.cpp src/audio_data.cpp src/player.cpp src/miniaudio.c)
#     target_include_directories(tunes_conversion_test PRIVATE include)
#     target_link_libraries(tunes_conversion_test PRIVATE PkgConfig::DEPS)
#     target_link_options(tunes_conversion_test PRIVATE
#         -Wl,--wrap=ma_device_init -Wl,--wrap=ma_device_uninit
#         -Wl,--wrap=ma_device_start -Wl,--wrap=ma_device_stop)
#     add_test(NAME audio_conversion COMMAND tunes_conversion_test)
#     set_tests_properties(audio_conversion PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
#     add_executable(tunes_library_scrollbar_test tests/library_scrollbar_test.cpp)
#     target_include_directories(tunes_library_scrollbar_test PRIVATE include)
#     add_test(NAME library_scrollbar COMMAND tunes_library_scrollbar_test)
#     add_executable(tunes_egl_window_test tests/egl_window_test.cpp src/client/egl_window.cpp)
#     target_include_directories(tunes_egl_window_test PRIVATE include)
#     target_link_libraries(tunes_egl_window_test PRIVATE tunes_drawing PkgConfig::DEPS)
#     add_test(NAME egl_window COMMAND tunes_egl_window_test)
#     set_tests_properties(egl_window PROPERTIES TIMEOUT 30 ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1")
#     add_executable(tunes_opengl_drawing_test tests/opengl_drawing_test.cpp)
#     target_include_directories(tunes_opengl_drawing_test PRIVATE include)
#     target_link_libraries(tunes_opengl_drawing_test PRIVATE tunes_drawing PkgConfig::DEPS)
#     add_test(NAME opengl_drawing COMMAND tunes_opengl_drawing_test)
#     set_tests_properties(opengl_drawing PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
#     add_executable(tunes_drawing_test tests/drawing_test.cpp)
#     target_include_directories(tunes_drawing_test PRIVATE include)
#     target_link_libraries(tunes_drawing_test PRIVATE tunes_drawing PkgConfig::DEPS)
#     add_test(NAME drawing COMMAND tunes_drawing_test)
#     add_executable(tunes_popup_overlay_test tests/popup_overlay_test.cpp src/events.cpp src/container.cpp)
#     target_include_directories(tunes_popup_overlay_test PRIVATE include)
#     target_link_libraries(tunes_popup_overlay_test PRIVATE PkgConfig::DEPS)
#     add_test(NAME popup_overlay COMMAND tunes_popup_overlay_test)
#     add_executable(tunes_scroll_hover_test tests/scroll_hover_test.cpp src/events.cpp src/container.cpp)
#     target_include_directories(tunes_scroll_hover_test PRIVATE include)
#     target_link_libraries(tunes_scroll_hover_test PRIVATE PkgConfig::DEPS)
#     add_test(NAME scroll_hover COMMAND tunes_scroll_hover_test)
#     add_executable(tunes_playback_queue_test tests/playback_queue_test.cpp)
#     target_include_directories(tunes_playback_queue_test PRIVATE include)
#     add_test(NAME playback_queue COMMAND tunes_playback_queue_test)
#     add_executable(tunes_pipewire_settings_test tests/pipewire_settings_test.cpp src/pipewire_settings.cpp)
#     target_include_directories(tunes_pipewire_settings_test PRIVATE include)
#     target_link_libraries(tunes_pipewire_settings_test PRIVATE PkgConfig::DEPS)
#     add_test(NAME pipewire_settings COMMAND tunes_pipewire_settings_test)
#     set_tests_properties(pipewire_settings PROPERTIES TIMEOUT 10)
#     add_executable(tunes_album_art_test tests/album_art_test.cpp src/album_art.cpp)
#     target_include_directories(tunes_album_art_test PRIVATE include)
#     target_link_libraries(tunes_album_art_test PRIVATE PkgConfig::DEPS)
#     add_test(NAME album_art_cache COMMAND tunes_album_art_test)
#     add_executable(tunes_session_test tests/session_state_test.cpp src/session_state.cpp src/audio_data.cpp src/audio_conversion.cpp src/miniaudio.c)
#     target_include_directories(tunes_session_test PRIVATE include)
#     target_link_libraries(tunes_session_test PRIVATE PkgConfig::DEPS)
#     add_test(NAME session_state COMMAND tunes_session_test)
#     add_executable(tunes_session_settings_test tests/session_settings_test.cpp src/session_state.cpp src/audio_data.cpp src/audio_conversion.cpp src/miniaudio.c)
#     target_include_directories(tunes_session_settings_test PRIVATE include)
#     target_link_libraries(tunes_session_settings_test PRIVATE PkgConfig::DEPS)
#     add_test(NAME session_settings COMMAND tunes_session_settings_test)
#     add_executable(tunes_player_session_test tests/player_session_test.cpp src/player.cpp src/miniaudio.c src/audio_conversion.cpp)
#     target_include_directories(tunes_player_session_test PRIVATE include)
#     target_link_libraries(tunes_player_session_test PRIVATE PkgConfig::DEPS)
#     target_link_options(tunes_player_session_test PRIVATE
#         -Wl,--wrap=ma_device_init -Wl,--wrap=ma_device_uninit
#         -Wl,--wrap=ma_device_start -Wl,--wrap=ma_device_stop)
#     add_test(NAME paused_session_restore COMMAND tunes_player_session_test)
#     add_executable(tunes_sample_rate_test tests/player_sample_rate_test.cpp src/player.cpp src/miniaudio.c src/audio_conversion.cpp)
#     target_include_directories(tunes_sample_rate_test PRIVATE include)
#     target_link_libraries(tunes_sample_rate_test PRIVATE PkgConfig::DEPS)
#     target_link_options(tunes_sample_rate_test PRIVATE -Wl,--wrap=ma_device_init -Wl,--wrap=ma_device_start)
#     add_test(NAME sample_rate_switching COMMAND tunes_sample_rate_test)
#     set_tests_properties(sample_rate_switching PROPERTIES TIMEOUT 20)
#     add_executable(tunes_player_pause_test tests/player_pause_test.cpp src/player.cpp src/miniaudio.c src/audio_conversion.cpp)
#     target_include_directories(tunes_player_pause_test PRIVATE include)
#     target_link_libraries(tunes_player_pause_test PRIVATE PkgConfig::DEPS)
#     target_link_options(tunes_player_pause_test PRIVATE
#         -Wl,--wrap=ma_device_init -Wl,--wrap=ma_device_start)
#     add_test(NAME paused_audio_buffer COMMAND tunes_player_pause_test)
#     set_tests_properties(paused_audio_buffer PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77)
# endif()

install(TARGETS tunes
        DESTINATION /usr/local/bin/)

# Offscreen visualizer check: cmake --build build --target tunes_visualizer_test
add_executable(tunes_visualizer_test EXCLUDE_FROM_ALL tests/visualizer_test.cpp src/visualizer.cpp
               src/player.cpp src/miniaudio.c src/audio_conversion.cpp)
target_include_directories(tunes_visualizer_test PRIVATE include)
target_compile_definitions(tunes_visualizer_test PRIVATE TUNES_VISUALIZER_SOURCE_DIR="${TUNES_DEPS_DIR}")
target_link_libraries(tunes_visualizer_test PRIVATE tunes_drawing libprojectM::projectM PkgConfig::DEPS)
