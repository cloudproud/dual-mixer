cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Required by cmake/linux/defaults.cmake when libobs is found without CMake package files.
set(ENABLE_FRONTEND_API ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# The plugin is a frontend plugin: it needs libobs (canvases, outputs), the
# frontend API (docks, scenes, native output control) and Qt (workspace UI).
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui)

# Qt's FindWrapOpenGL adds AGL.framework, which no longer exists in the macOS 26 SDK.
# Nothing here uses AGL, so drop it from the interface to keep linking with newer SDKs.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
  get_target_property(_wrap_opengl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
  if(_wrap_opengl_libs)
    list(FILTER _wrap_opengl_libs EXCLUDE REGEX "AGL")
    set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${_wrap_opengl_libs}")
  endif()
endif()

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets
)

target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/common.hpp
    src/common.cpp
    src/mix.hpp
    src/mix.cpp
    src/destination.hpp
    src/destination.cpp
    src/manager.hpp
    src/manager.cpp
    src/ui/display-widget.hpp
    src/ui/display-widget.cpp
    src/ui/flow-layout.hpp
    src/ui/flow-layout.cpp
    src/ui/properties-form.hpp
    src/ui/properties-form.cpp
    src/ui/projector-window.hpp
    src/ui/projector-window.cpp
    src/ui/scene-list.hpp
    src/ui/scene-list.cpp
    src/ui/ui-helpers.hpp
    src/ui/ui-helpers.cpp
    src/ui/mix-panel.hpp
    src/ui/mix-panel.cpp
    src/ui/workspace-dock.hpp
    src/ui/workspace-dock.cpp
    src/ui/mix-settings-dialog.hpp
    src/ui/mix-settings-dialog.cpp
    src/ui/destination-dialog.hpp
    src/ui/destination-dialog.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
