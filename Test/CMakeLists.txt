# Renderx 渲染引擎单元测试配置
#
# 测试分为两个目标：
# 1. RenderxTests：纯数据结构测试，不链接任何后端，适用于 CI/CD
# 2. RenderxGLTests：链接完整 RHI（Null + GL 后端）与 RT 层
#
# 为什么 RHI/RT 的契约测试在「GL」目标里跑 Null 后端：
# rhiFactory.cpp 里 createDevice 会引用 createGlDevice，链接期必须提供
# GL 后端与 glLoader（以及 opengl32 / 系统 GL 库）。拆成两个工厂只为测试
# 服务会让产品代码多出一条分支，不值得。

find_package(GTest REQUIRED)
include(GoogleTest)

# ============================================================
# Target 1: RenderxTests (纯头文件数据结构，无后端依赖)
# ============================================================

set(RENDERX_NULL_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/ArenaTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SlotMapTests.cpp"
)

# 已删除：NullBackendTests.cpp / RuntimeSessionTests.cpp —— 它们测的是旧
# RHI（rhiNull.h）与旧 RT C API（runtime_session.h），两者均已删除。
# 等价覆盖分别落在 RhiCoreTests.cpp（新 RHI 契约）与 RxRuntimeTests.cpp
# （renderx.h 的 rx* 接口）。

add_executable(RenderxTests ${RENDERX_NULL_TEST_SOURCES})

target_compile_features(RenderxTests PRIVATE cxx_std_17)

target_compile_definitions(RenderxTests PRIVATE RENDER_EXPORTS)

target_include_directories(RenderxTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

# Renderx 已不依赖 Log：日志一律通过 rxLogCallback 交回宿主，
# 因此测试目标也不需要链接业务日志库。
target_link_libraries(RenderxTests PRIVATE
    GTest::gtest
    GTest::gtest_main
    RenderxShaders
)

gtest_discover_tests(RenderxTests)

# ============================================================
# Target 2: RenderxGLTests (完整 RHI + RT；测试本身跑 Null 后端)
# ============================================================

set(RENDERX_GL_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/RhiCoreTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/RxRuntimeTests.cpp"
)

set(RENDERX_GL_INTERNAL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/platform/glLoader.cpp"
    # RHI：工厂 + Null 后端 + GL 后端
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rhi/rhiFactory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rhi/null/nullBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rhi/gl/glDevice.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rhi/gl/glCommandList.cpp"
    # RT：Runtime / Session / 增量渲染 / 字体 / C API
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rt/rxRuntime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rt/rxSession.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rt/rxIncremental.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/rt/rxFont.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/c_api/rxCApi.cpp"
)

add_executable(RenderxGLTests ${RENDERX_GL_TEST_SOURCES} ${RENDERX_GL_INTERNAL_SOURCES})

target_compile_features(RenderxGLTests PRIVATE cxx_std_17)

target_compile_definitions(RenderxGLTests PRIVATE RENDER_EXPORTS)

# 字体用例需要一份真实 TTF。DLL 自己不做文件 IO，但测试可以——
# 这里把路径以宏注入，避免依赖测试进程的工作目录。
target_compile_definitions(RenderxGLTests PRIVATE
    RENDERX_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../src/res/default_screen_font.ttf"
)

target_include_directories(RenderxGLTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

# rxFont.cpp 直接 include <stb_truetype.h>，测试目标也要能找到它
if(STB_TRUETYPE_INCLUDE_DIR)
    target_include_directories(RenderxGLTests PRIVATE ${STB_TRUETYPE_INCLUDE_DIR})
elseif(DEFINED VCPKG_DIR)
    file(GLOB _vcpkg_stb_incs "${VCPKG_DIR}/installed/*/include")
    list(REMOVE_DUPLICATES _vcpkg_stb_incs)
    target_include_directories(RenderxGLTests PRIVATE ${_vcpkg_stb_incs})
endif()

if(WIN32)
    target_link_libraries(RenderxGLTests PRIVATE opengl32)
endif()

target_link_libraries(RenderxGLTests PRIVATE
    GTest::gtest
    GTest::gtest_main
    RenderxShaders
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
