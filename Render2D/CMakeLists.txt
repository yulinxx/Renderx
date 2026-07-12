cmake_minimum_required(VERSION 4.3)

# --- 模块元信息 ---
set(LIB_NAME "Render2D")
set(LIB_FOLDER "Render")
set(LIB_DESCRIPTION "Render 2D Library")
set(LIB_EXPORT_MACRO "RENDER_EXPORTS")

# --- 版本（按模块独立）---
set(VERSION_MAJOR 1)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)
set(PROJECT_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

# --- 路径 ---
if(NOT SANYI_ROOT)
    get_filename_component(SANYI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
include("${SANYI_ROOT}/CMake/SanYiPaths.cmake")
include("${SANYI_ROOT}/CMake/Utils.cmake")

# --- 源文件 ---
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.cxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.c"
)
file(GLOB_RECURSE LIB_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.hpp"
)
list(FILTER LIB_SOURCES EXCLUDE REGEX "${SANYI_SOURCE_EXCLUDE_REGEX}")
list(FILTER LIB_HEADERS EXCLUDE REGEX "${SANYI_HEADER_EXCLUDE_REGEX}")
list(LENGTH LIB_SOURCES LIB_SOURCES_COUNT)
list(LENGTH LIB_HEADERS LIB_HEADERS_COUNT)

# --- 第三方依赖 (Qt 版本分支 + Stb) ---
list(INSERT CMAKE_PREFIX_PATH 0 "${Qt_INSTALL_DIR}")

find_package(Qt6 COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets REQUIRED)
    set(QT_LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)

find_package(Stb REQUIRED)

# --- 目标定义 ---
add_library(${LIB_NAME} SHARED
    ${LIB_SOURCES}
    ${LIB_HEADERS}
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${LIB_SOURCES} ${LIB_HEADERS})

target_include_directories(${LIB_NAME}
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Include"
        "${CMAKE_CURRENT_SOURCE_DIR}/Include/Render2D"
    PRIVATE
        "${SANYI_RENDER_COMMON_DIR}/Include"
        "${SANYI_RENDER_COMMON_DIR}/Include/Render"
        "${SANYI_ENGINE_COMMON_DIR}/Include"
        "${SANYI_ENGINE_2D_DIR}/Include"
)

target_compile_features(${LIB_NAME} PUBLIC cxx_std_17)
target_compile_definitions(${LIB_NAME} PRIVATE ${LIB_EXPORT_MACRO})

# --- 目标属性 ---
set_target_properties(${LIB_NAME} PROPERTIES
    AUTOMOC ON
    AUTOUIC OFF
    AUTORCC ON
    FOLDER "${LIB_FOLDER}"
    DEBUG_POSTFIX "_d"
)

# --- 编译选项 ---
if(MSVC)
    target_compile_options(${LIB_NAME} PRIVATE /utf-8 /FS)
else()
    target_compile_options(${LIB_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- 链接 ---
target_link_libraries(${LIB_NAME}
    PUBLIC
        Utility
        EngineCommon
        Engine2D
        RenderCommon
        ${QT_LIBRARIES}
)

# --- 版本信息 ---
sanyi_add_version_info(${LIB_NAME} "${LIB_NAME}" "${LIB_DESCRIPTION}")

# --- 构建信息 ---
message(STATUS "[${LIB_NAME}]")
message(STATUS "  Version: ${PROJECT_VERSION}")
# message(STATUS "  Sources: ${LIB_SOURCES_COUNT}  Headers: ${LIB_HEADERS_COUNT}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}(${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "----------------------------------------")
