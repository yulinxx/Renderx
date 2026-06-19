cmake_minimum_required(VERSION 3.20)

set(LIB_NAME "RenderCommon")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMake/Utils.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/../../CMake/Utils.cmake")
endif()

file(GLOB_RECURSE RENDER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.cxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.c"
)

file(GLOB_RECURSE RENDER_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.hpp"
)

add_library(${LIB_NAME} SHARED
    ${RENDER_SOURCES}
    ${RENDER_HEADERS}
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${RENDER_SOURCES} ${RENDER_HEADERS})

target_include_directories(${LIB_NAME}
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Include"
        "${CMAKE_CURRENT_SOURCE_DIR}/Include/Render"
    PRIVATE
        "${CMAKE_SOURCE_DIR}/Engine/Common/Include"
        "${CMAKE_SOURCE_DIR}/Engine/Engine2D/Include"
        "${CMAKE_SOURCE_DIR}/Engine/Engine3D/Include"
)

target_compile_features(${LIB_NAME} PUBLIC cxx_std_17)

list(INSERT CMAKE_PREFIX_PATH 0 "${Qt_INSTALL_DIR}")

if(QT_VERSION_MAJOR STREQUAL "6")
    find_package(Qt6 COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets REQUIRED)
    set(QT_LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
else()
    find_package(Qt5 COMPONENTS Core Gui Widgets OpenGL REQUIRED)
    set(QT_LIBRARIES Qt5::Core Qt5::Gui Qt5::Widgets Qt5::OpenGL)
endif()

set_target_properties(${LIB_NAME} PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    FOLDER "Render"
)

target_link_libraries(${LIB_NAME}
    PUBLIC
        Utility
        EngineCommon
        Engine2D
        Engine3D
        ${QT_LIBRARIES}
)

target_compile_definitions(${LIB_NAME} PRIVATE RENDER_EXPORTS)

if(MSVC)
    target_compile_options(${LIB_NAME} PRIVATE /utf-8 /FS)
endif()

set(LIB_VERSION_MAJOR 1)
set(LIB_VERSION_MINOR 0)
set(LIB_VERSION_PATCH 0)
set(LIB_VERSION "${LIB_VERSION_MAJOR}.${LIB_VERSION_MINOR}.${LIB_VERSION_PATCH}")

set_target_properties(${LIB_NAME} PROPERTIES VERSION ${LIB_VERSION})

string(TOUPPER ${LIB_NAME} _VER_PREFIX)
target_compile_definitions(${LIB_NAME} PUBLIC
    ${_VER_PREFIX}_VERSION_MAJOR=${LIB_VERSION_MAJOR}
    ${_VER_PREFIX}_VERSION_MINOR=${LIB_VERSION_MINOR}
    ${_VER_PREFIX}_VERSION_PATCH=${LIB_VERSION_PATCH}
    ${_VER_PREFIX}_VERSION_STRING="${LIB_VERSION}"
)

sanyi_add_version_info(${LIB_NAME} "${LIB_NAME}" "Render Common Library")

message(STATUS "[${LIB_NAME}]")
message(STATUS "  Library: ${LIB_NAME}")
message(STATUS "  Version: ${LIB_VERSION}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}(${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "----------------------------------------")