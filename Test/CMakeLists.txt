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
    "${CMAKE_CURRENT_SOURCE_DIR}/Mat4Tests.cpp"
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

# Metal 后端只在 Apple 平台编译，而 rhiFactory.cpp 在 Apple 上会引用
# createMetalDevice —— 测试目标必须一并链接这些源文件，否则 macOS 上会
# 出现「未定义的符号 createMetalDevice」。非 Apple 平台无此问题。
# 源文件直接复用父级 CMakeLists 收集的 RENDER_OBJC_SOURCES（src 下全部
# .mm，即 Metal 后端），避免在测试里再硬编码一遍文件路径。
if(APPLE)
    list(APPEND RENDERX_GL_INTERNAL_SOURCES ${RENDER_OBJC_SOURCES})
    # 与主目标一致：Metal 源开启 ARC（只能按源文件设置）。
    # 不设置 LANGUAGE CXX：clang++ 按 .mm 扩展名自动按 Objective-C++ 解析；
    # 设置后 CMake 会追加 -x c++ 导致 ObjC 头编译失败。
    set_source_files_properties(${RENDER_OBJC_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc"
        SKIP_UNITY_BUILD_INCLUSION ON
    )
endif()

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

# Apple 平台测试目标也要链接 Metal 框架：它编译了 metalDevice.mm
if(APPLE)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    target_link_libraries(RenderxGLTests PRIVATE
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${APPKIT_FRAMEWORK}
    )
endif()

target_link_libraries(RenderxGLTests PRIVATE
    GTest::gtest
    GTest::gtest_main
    RenderxShaders
)

gtest_discover_tests(RenderxGLTests)

# ============================================================
# 通用设置
# ============================================================

set_property(TARGET RenderxTests RenderxGLTests PROPERTY FOLDER "Tests")

# 关闭 unity build：RhiCoreTests.cpp 用 using namespace Render::RHI，
# RxRuntimeTests.cpp 用 using namespace Render::RT，合并进同一翻译单元后
# Capabilities / BufferDesc / SurfaceDesc / FrameStats 等同名类型全部歧义。
# 与 Engine2DTests 的处理一致（测试目标不需要 unity 的编译加速）。
set_target_properties(RenderxGLTests PROPERTIES UNITY_BUILD OFF)

if(MSVC)
    target_compile_options(RenderxTests PRIVATE /utf-8)
    target_compile_options(RenderxGLTests PRIVATE /utf-8)
endif()

# ============================================================
# Target 3: RenderxMetalTests (仅 Apple：Metal 后端的离屏渲染正确性)
# ============================================================
#
# 与 RenderxGLTests 的区别：那边跑 Null 后端验证 RHI/RT 契约，这边跑真实
# Metal 设备并断言**像素结果**（清屏色、三角形覆盖、纹理采样、3D 深度遮挡、
# 光照方向跟随、双面光照）。这是 Metal 后端唯一能证明「画得对」的地方——
# 契约测试即使一条命令都没提交也全绿。
#
# 只在 Apple 平台存在：Metal 后端源文件本身就不参与其他平台的编译。
#
# 注：本段曾在一轮外部改写中丢失（文件与 RenderxMetalTests.mm 都是未跟踪
# 状态，注册段不在基线里），导致该测试目标凭空消失、M3 的 Metal 回归证据
# 无法复现。此处按 RenderxGLTests 的同构配置恢复。
if(APPLE)
    add_executable(RenderxMetalTests
        "${CMAKE_CURRENT_SOURCE_DIR}/RenderxMetalTests.mm"
    )

    target_compile_features(RenderxMetalTests PRIVATE cxx_std_17)
    target_compile_definitions(RenderxMetalTests PRIVATE RENDER_EXPORTS)
    # 字体用例需要一份真实 TTF：DLL 自己不做文件 IO，但测试可以——
    # 与 RenderxGLTests 用同一个宏注入路径。
    target_compile_definitions(RenderxMetalTests PRIVATE
        RENDERX_TEST_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../src/res/default_screen_font.ttf"
    )

    target_include_directories(RenderxMetalTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    )

    if(STB_TRUETYPE_INCLUDE_DIR)
        target_include_directories(RenderxMetalTests PRIVATE ${STB_TRUETYPE_INCLUDE_DIR})
    elseif(DEFINED VCPKG_DIR)
        file(GLOB _vcpkg_stb_incs_metal "${VCPKG_DIR}/installed/*/include")
        list(REMOVE_DUPLICATES _vcpkg_stb_incs_metal)
        target_include_directories(RenderxMetalTests PRIVATE ${_vcpkg_stb_incs_metal})
    endif()

    # 走公共 C API（rx*），因此链接 RenderX 动态库而不是重编内部源；
    # 离屏渲染仍需 AppKit 的 NSView 与 Metal/QuartzCore。
    find_library(RENDERX_METAL_FRAMEWORK Metal REQUIRED)
    find_library(RENDERX_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(RENDERX_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(RENDERX_APPKIT_FRAMEWORK AppKit REQUIRED)

    target_link_libraries(RenderxMetalTests PRIVATE
        RenderX
        GTest::gtest
        GTest::gtest_main
        ${RENDERX_METAL_FRAMEWORK}
        ${RENDERX_QUARTZCORE_FRAMEWORK}
        ${RENDERX_FOUNDATION_FRAMEWORK}
        ${RENDERX_APPKIT_FRAMEWORK}
    )

    set_target_properties(RenderxMetalTests PROPERTIES UNITY_BUILD OFF)
    set_property(TARGET RenderxMetalTests PROPERTY FOLDER "Tests")
    gtest_discover_tests(RenderxMetalTests)
endif()
