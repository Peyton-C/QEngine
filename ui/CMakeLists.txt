cmake_minimum_required(VERSION 3.25)

project(qengine_ui VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

include(FetchContent)

# A system SDL3 (Homebrew, or a distro's libsdl3-dev) is used when there is
# one, since building SDL is most of the build time. Dear ImGui ships no CMake
# and nlohmann/json is header only, so both are always fetched. Point
# FETCHCONTENT_SOURCE_DIR_<NAME> at a local checkout to build offline.
find_package(SDL3 3.2 CONFIG QUIET)

if(NOT SDL3_FOUND)
  FetchContent_Declare(sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.18
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(sdl3)
endif()

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.8
  GIT_SHALLOW TRUE)

FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)

FetchContent_MakeAvailable(imgui json)

add_library(qengine_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp)

target_include_directories(qengine_imgui PUBLIC ${imgui_SOURCE_DIR})
# Drawing code is all ImVec2 arithmetic.
target_compile_definitions(qengine_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(qengine_imgui PUBLIC SDL3::SDL3)

add_executable(qengine-ui
  src/main.cpp
  src/Layout.cpp
  src/Surface.cpp
  src/Texture.cpp
  src/Touch.cpp)

# AppKit only gives a touchscreen's fingers to gesture recognizers, which SDL
# does not install, so macOS gets one of its own.
if(APPLE)
  enable_language(OBJCXX)
  target_sources(qengine-ui PRIVATE src/TouchMac.mm)
  set_source_files_properties(src/TouchMac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(qengine-ui PRIVATE "-framework AppKit")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(qengine-ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Defaults for running from the build tree: the layouts beside this file, and
# the photos in QEngine's untracked build/ directory, since they are inMusic's.
target_compile_definitions(qengine-ui PRIVATE
  QENGINE_UI_LAYOUTS="${CMAKE_CURRENT_SOURCE_DIR}/layouts"
  QENGINE_UI_PHOTOS="${CMAKE_CURRENT_SOURCE_DIR}/../build/product_photos")

target_link_libraries(qengine-ui PRIVATE qengine_imgui nlohmann_json::nlohmann_json)
