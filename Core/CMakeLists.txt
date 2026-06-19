cmake_minimum_required(VERSION 3.20)

set(TARGET_NAME RenderCore)

# 源文件
set(INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Include)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Src)
set(RENDER_CORE_INC ${INC_DIR}/RenderCore)

set(HEADERS
    ${INC_DIR}/RenderCore.h
    ${RENDER_CORE_INC}/IRenderBackend.h
    ${RENDER_CORE_INC}/RenderWorld.h
    ${RENDER_CORE_INC}/RenderBuffer.h
    ${RENDER_CORE_INC}/RenderBatch.h
    ${RENDER_CORE_INC}/RenderView.h
    ${RENDER_CORE_INC}/GL46Backend.h
    ${RENDER_CORE_INC}/RenderWidgetEx.h
)

set(SOURCES
    ${SRC_DIR}/GL46Backend.cpp
    ${SRC_DIR}/RenderBatch.cpp
    ${SRC_DIR}/RenderWorld.cpp
    ${SRC_DIR}/RenderBuffer.cpp
    ${SRC_DIR}/RenderView.cpp
    ${SRC_DIR}/RenderWidgetEx.cpp
)

# 创建库
add_library(${TARGET_NAME} STATIC
    ${HEADERS}
    ${SOURCES}
)

# 包含目录
target_include_directories(${TARGET_NAME} PUBLIC
    ${INC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include
)

# 链接OpenGL库
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
target_link_libraries(${TARGET_NAME} PUBLIC
    OpenGL::GL
    GLEW::GLEW
)

# 链接Qt（用于Qt集成）
if(QT_VERSION_MAJOR STREQUAL "6")
    find_package(Qt6 COMPONENTS OpenGL OpenGLWidgets Widgets REQUIRED)
    target_link_libraries(${TARGET_NAME} PUBLIC
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::Widgets
    )
else()
    find_package(Qt5 COMPONENTS OpenGL Widgets REQUIRED)
    target_link_libraries(${TARGET_NAME} PUBLIC
        Qt5::OpenGL
        Qt5::Widgets
    )
endif()

# C++20
target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)

# 导出宏（与 RenderCommon 共享 RENDER_API）
target_compile_definitions(${TARGET_NAME} PRIVATE RENDER_EXPORTS)

# 导出
set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME ${TARGET_NAME}
    POSITION_INDEPENDENT_CODE ON
)
