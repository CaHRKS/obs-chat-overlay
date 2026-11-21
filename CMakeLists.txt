cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.cpp
          src/chat.cpp
          src/json.cpp
          src/net.cpp
          src/images.cpp
          src/render.cpp
          src/theme.cpp
          src/twitch.cpp
)

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES CXX_STANDARD 17
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF
)

# Windows system libraries used by the renderer (Direct2D / DirectWrite / WIC) and
# by the network layer (WinHTTP). No third-party dependencies are required.
target_link_libraries(
  ${CMAKE_PROJECT_NAME} PRIVATE d2d1 dwrite windowscodecs winhttp ole32 shlwapi
)

if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /utf-8 /WX- /wd4100)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
