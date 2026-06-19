cmake_minimum_required(VERSION 3.20)

include(${CMAKE_CURRENT_SOURCE_DIR}/../Config.txt)

if(DEFINED VCPKG_DIR AND EXISTS "${VCPKG_DIR}")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake")
endif()

project(SanYiRender
    VERSION 2.0.0
    DESCRIPTION "SanYi CAD Render Library (Industrial-Grade 2D/3D Rendering)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/wd4251)
    add_compile_options(/wd4244)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options(-Wno-conversion)
    add_compile_options(-Wno-float-conversion)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# ==================== Source Files ====================

set(RENDER_PUBLIC_HEADERS
    include/render/render.h
    include/render/render_types.h
)

set(RENDER_SOURCES
    src/c_api/render_c_api.cpp
    src/platform/gl_loader.cpp
    src/rhi/rhi_gl.cpp
    src/core/render_world.cpp
    src/core/batch_queue.cpp
    src/core/overlay_queue.cpp
    src/core/mesh_manager.cpp
    src/core/text_atlas.cpp
    src/core/scene_env.cpp
    src/core/stb_truetype_impl.cpp
    src/adapter/render_widget_2d.cpp
    src/adapter/render_widget_3d.cpp
    src/shader/shaders.cpp
)

# ==================== Find Packages ====================

if(QT_VERSION_MAJOR EQUAL 6)
    find_package(Qt6 COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets REQUIRED)
else()
    find_package(Qt5 5.15 COMPONENTS Core Gui Widgets OpenGL REQUIRED)
endif()

find_package(Stb QUIET)

if(WIN32)
    set(OPENGL_LIBRARIES opengl32)
elseif(UNIX AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    set(OPENGL_LIBRARIES OpenGL::GL)
else()
    find_package(OpenGL REQUIRED)
    set(OPENGL_LIBRARIES OpenGL::GL)
endif()

# ==================== Library ====================

add_library(${PROJECT_NAME} ${RENDER_SOURCES})

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        RENDER_EXPORTS
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${OPENGL_LIBRARIES}
)

if(Stb_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${Stb_INCLUDE_DIR})
elseif(DEFINED VCPKG_DIR AND EXISTS "${VCPKG_DIR}/installed/x64-windows/include")
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${VCPKG_DIR}/installed/x64-windows/include")
endif()

if(QT_VERSION_MAJOR EQUAL 6)
    target_link_libraries(${PROJECT_NAME}
        PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::OpenGL
            Qt6::OpenGLWidgets
    )
else()
    target_link_libraries(${PROJECT_NAME}
        PRIVATE
            Qt5::Core
            Qt5::Gui
            Qt5::Widgets
            Qt5::OpenGL
    )
endif()

# ==================== Install ====================

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/render
    DESTINATION include
)

message(STATUS "[${PROJECT_NAME}]")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Shared Libraries: ${BUILD_SHARED_LIBS}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
