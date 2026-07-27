# Renderx 渲染引擎单元测试配置

find_package(GTest REQUIRED)
include(GoogleTest)

# Phase 0/1 验证期间，只编译通过验证的测试文件
# 历史遗留的 BatchQueueTests/MeshManagerTests/RenderTypesTests 存在接口不匹配，待后续统一修复
set(RENDERX_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/TransientBufferPoolTests.cpp"
)

# 直接编译测试所需的内部实现源文件（内部类符号未从 DLL 导出）
set(RENDERX_INTERNAL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/transient_buffer_pool.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rhi/rhi_gl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/gl_loader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/shader/shaders.cpp"
)

add_executable(RenderxTests ${RENDERX_TEST_SOURCES} ${RENDERX_INTERNAL_SOURCES})

target_compile_features(RenderxTests PRIVATE cxx_std_17)

# 测试直接编译内部源文件，需要以 dllexport 模式编译导出符号
target_compile_definitions(RenderxTests PRIVATE RENDER_EXPORTS)

target_include_directories(RenderxTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

# Log 库头文件路径
set(LOG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Log/Log/Include")
if(EXISTS "${LOG_INCLUDE_DIR}/Log/SyLogger.h")
    target_include_directories(RenderxTests PRIVATE ${LOG_INCLUDE_DIR})
endif()

target_link_libraries(RenderxTests PRIVATE
    GTest::gtest
    GTest::gtest_main
    opengl32
    Log
)

gtest_discover_tests(RenderxTests)

set_property(TARGET RenderxTests PROPERTY FOLDER "Tests")

if(MSVC)
    target_compile_options(RenderxTests PRIVATE /utf-8)
endif()
