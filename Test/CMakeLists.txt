# Renderx 渲染引擎单元测试配置
#
# 测试分为两个目标：
# 1. RenderxTests：Null backend，无需 GPU/OpenGL 上下文，适用于 CI/CD
# 2. RenderxGLTests：OpenGL backend，需要 OpenGL 上下文，仅在本地开发环境运行

find_package(GTest REQUIRED)
include(GoogleTest)

# ============================================================
# Target 1: RenderxTests (Null backend, no GPU required)
# ============================================================

set(RENDERX_NULL_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/ArenaTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SlotMapTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/NullBackendTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/RuntimeSessionTests.cpp"
)

# Null backend 仅需要 rhiNull.cpp，不依赖 OpenGL。
# 注意：源文件名必须与磁盘上的大小写完全一致。此前这里写的是
# rhinull.cpp / renderruntime.cpp / transientbufferpool.cpp / rhigl.cpp /
# glloader.cpp（全小写），仅在 macOS/Windows 的大小写不敏感文件系统上
# 侥幸解析成功，在 Linux 上会直接配置失败。
set(RENDERX_NULL_INTERNAL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rhi/rhiNull.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/shader/shaders.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/pipelineStateManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/rendererRuntime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/rendererSession.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/c_api/renderCApiRuntime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StubRHIFactories.cpp"
)

add_executable(RenderxTests ${RENDERX_NULL_TEST_SOURCES} ${RENDERX_NULL_INTERNAL_SOURCES})

target_compile_features(RenderxTests PRIVATE cxx_std_17)

target_compile_definitions(RenderxTests PRIVATE RENDER_EXPORTS)

target_include_directories(RenderxTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

# Log 库：如果作为子目录构建，target_link_libraries 会自动传播 include 目录
# 否则回退到显式添加头文件路径
if(NOT TARGET Log)
    set(LOG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Log/Log/Include")
    if(EXISTS "${LOG_INCLUDE_DIR}/Log/SyLogger.h")
        target_include_directories(RenderxTests PRIVATE ${LOG_INCLUDE_DIR})
    endif()
endif()

target_link_libraries(RenderxTests PRIVATE
    GTest::gtest
    GTest::gtest_main
    Log
)

gtest_discover_tests(RenderxTests)

# ============================================================
# Target 2: RenderxGLTests (OpenGL backend, requires GPU)
# ============================================================

set(RENDERX_GL_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/TransientBufferPoolTests.cpp"
)

# OpenGL tests 需要完整的 GL 栈
set(RENDERX_GL_INTERNAL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/transientBufferPool.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rhi/rhiGl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/glLoader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/shader/shaders.cpp"
)

add_executable(RenderxGLTests ${RENDERX_GL_TEST_SOURCES} ${RENDERX_GL_INTERNAL_SOURCES})

target_compile_features(RenderxGLTests PRIVATE cxx_std_17)

target_compile_definitions(RenderxGLTests PRIVATE RENDER_EXPORTS)

target_include_directories(RenderxGLTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

if(NOT TARGET Log)
    set(LOG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Log/Log/Include")
    if(EXISTS "${LOG_INCLUDE_DIR}/Log/SyLogger.h")
        target_include_directories(RenderxGLTests PRIVATE ${LOG_INCLUDE_DIR})
    endif()
endif()

if(WIN32)
    target_link_libraries(RenderxGLTests PRIVATE opengl32)
endif()

target_link_libraries(RenderxGLTests PRIVATE
    GTest::gtest
    GTest::gtest_main
    Log
)

gtest_discover_tests(RenderxGLTests)

# ============================================================
# Common settings
# ============================================================

set_property(TARGET RenderxTests RenderxGLTests PROPERTY FOLDER "Tests")

if(MSVC)
    target_compile_options(RenderxTests PRIVATE /utf-8)
    target_compile_options(RenderxGLTests PRIVATE /utf-8)
endif()
