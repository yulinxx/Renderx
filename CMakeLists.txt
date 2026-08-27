# 指定CMake最低版本要求
cmake_minimum_required(VERSION 4.3)

# 包含上级目录的配置文件（通常包含全局变量定义）
include(${CMAKE_CURRENT_SOURCE_DIR}/../Config.cmake)

# 如果定义了VCPKG_DIR环境变量且路径存在，则使用vcpkg作为包管理器
if(DEFINED VCPKG_DIR AND EXISTS "${VCPKG_DIR}")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake")
endif()

# 项目配置
# - 项目名称: RenderX
# - 版本: 2.0.0
# - 描述: 工业级2D/3D渲染库
# - 语言: C++
project(RenderX
    VERSION 2.0.0
    DESCRIPTION "SanYi CAD Render Library (Industrial-Grade 2D/3D Rendering)"
    LANGUAGES CXX
)

# 模块路径（引入统一入口 sanyi_add_shared_library）
if(NOT SANYI_ROOT)
    get_filename_component(SANYI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()
include("${SANYI_ROOT}/CMake/SanYiPaths.cmake")
include("${SANYI_ROOT}/CMake/Utils.cmake")

# 设置C++标准为17，并强制要求支持该标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译器警告配置
if(MSVC)
    # Visual Studio编译器：启用最高警告级别，UTF-8编码
    add_compile_options(/W4 /utf-8)
    # 禁用安全警告（使用标准C库函数时）
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # 禁用类型转换警告（double转float等）
    add_compile_options(/wd4244)
else()
    # GCC/Clang编译器：启用所有警告
    add_compile_options(-Wall -Wextra -Wpedantic)
    # 禁用类型转换相关警告
    add_compile_options(-Wno-conversion)
    add_compile_options(-Wno-float-conversion)
endif()

# 如果未指定构建类型，默认为Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 注意：本目标始终显式 SHARED（见 sanyi_add_shared_library），
# 不存在静态库构建路径，因此这里不再定义 BUILD_SHARED_LIBS——
# 该选项此前是无效的死配置（Utils.cmake 无条件 add_library(SHARED)）。

# ==================== 源文件配置 ====================

# 公开头文件列表（对外暴露的API头文件）
#
# renderx.h 是**唯一**公共头（零 STL、全 POD、全 static_assert）。
#
# 已删除：render.h（62 个 C API，其中 4 个函数早已无定义）、
# TessParams.h（通过公共头共享 inline 离散化实现，是耦合而非封装）、
# RenderTypes.h 与 runtime_session.h（旧 RT C API；其能力已由 renderx.h
# 的 rx* 接口完整覆盖，且二者与 renderx.h 在 namespace Render::RT 中
# 有 11 个同名但不兼容的类型，无法共存）。
set(RENDER_PUBLIC_HEADERS
    include/render/renderx.h
)

# 源文件列表
# 源文件自动收集（新增 .cpp 文件无需修改此处）
file(GLOB_RECURSE RENDER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# Metal / Vulkan 后端将在新 RHI（rhiGpuDevice.h）之上重建，
# 届时按平台条件加入 src/rhi/metal/ 与 src/rhi/vulkan/ 目录。
# 旧的 rhiVulkan.cpp / rhiMetal.mm 已删除：前者 vkCreateShaderModule
# 从未调用（管线永远 0 个 shader stage），后者 include 了不存在的头文件、
# 类型错乱且从未进入编译。

# 统一入口要求源文件使用绝对路径（内部 source_group 用 RELATIVE_PATH 拆分），
# 将上面的相对路径统一归一化
set(RENDER_SOURCES_NORM)
foreach(_src IN LISTS RENDER_SOURCES)
    if(NOT IS_ABSOLUTE "${_src}")
        get_filename_component(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src}" ABSOLUTE)
    endif()
    list(APPEND RENDER_SOURCES_NORM "${_src}")
endforeach()
set(RENDER_SOURCES ${RENDER_SOURCES_NORM})
set(RENDER_HEADERS_NORM)
foreach(_hdr IN LISTS RENDER_PUBLIC_HEADERS)
    if(NOT IS_ABSOLUTE "${_hdr}")
        get_filename_component(_hdr "${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}" ABSOLUTE)
    endif()
    list(APPEND RENDER_HEADERS_NORM "${_hdr}")
endforeach()
set(RENDER_PUBLIC_HEADERS ${RENDER_HEADERS_NORM})

# ==================== Shader 嵌入 ====================
#
# shader 不再在运行时从磁盘加载，而是在构建期转成字节数组编入 DLL。
# 详细动机见 CMake/EmbedShaders.cmake 的文件头（macOS bundle 下路径推导
# 失败导致视口全黑、shader 非构建依赖导致调试时跑旧 shader）。
#
# 新增 shader 只需把文件加进本列表，无需改任何 C++ 代码。
# 语言按扩展名自动推断：.vert/.frag/.comp -> GLSL，.spv -> SPIR-V，
# .metallib -> Metal 库，.metal -> MSL。
set(RENDERX_SHADER_SOURCES
    # ---- RT 默认管线（DefaultPipeline 枚举对应）----
    # 这些此前是 rendererRuntime.cpp 里的 raw string 字面量。内联在 .cpp 中
    # 的 GLSL 无法被语法高亮、无法被构建工具交叉编译成 SPIR-V / MSL，
    # 也绕过了 src/shader/ 目录的存在意义，因此提取为独立文件。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/world_p3c3.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/world_p3c3.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/world_p3c4.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/world_p3c4.frag
    # 世界锚定 + 屏幕定尺寸（RenderSpace::WorldPinned，见新渲染架构 §15）。
    # 片段复用 world_p3c4.frag。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/world_pinned_p3o2c4.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/screen_p3c3.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/screen_p3c4.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/screen_p3c4.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/screen_tex_p2t2c4.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/screen_tex_p2t2c4.frag
    # 世界空间贴图（位图实体）：顶点是世界坐标，随视图变换。
    # 片元复用 screen_tex_p2t2c4.frag —— 它只用 vUV/vColor，与空间无关。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/world_tex_p3t2c4.vert
    # 字形四边形：顶点复用 screen_tex（同为 P2T2C4 + 屏幕空间），
    # 片元不同——图集是 R8 覆盖率，alpha 取 .r、rgb 取顶点色。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/screen_glyph_p2t2c4.frag
    # 世界空间字形：顶点复用 world_tex_p3t2c4.vert（同为 P3T2C4 + 世界空间），
    # 片元不同——图集是 R8 **距离场**而非覆盖率，靠 fwidth 求导做缩放无关的
    # 抗锯齿，一张图集因此能服务所有缩放（见 FontDesc::sdfPadding）。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/world_glyph_sdf_p3t2c4.frag

    # ---- 点图元 ----
    # 点必须有独立的顶点着色器：尺寸是像素量、只能写 gl_PointSize，
    # 无法用顶点几何表达（否则缩放时点会跟着变大）。
    # 片段着色器与空间无关，世界/屏幕共用。
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/world_point_p3c3.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/world_point_p3c4.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/screen_point_p3c3.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/screen_point_p3c4.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/point_p3c3.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/point_p3c4.frag

    # ---- 3D（Phase 11 收口 3D 时启用）----
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/mesh_3d.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/mesh_3d.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/mesh_3d_instanced.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/highlight_3d.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/highlight_3d.frag

    # ---- GPU 剔除 ----
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/culling.comp
)
# 已删除：scene_2d.* / overlay.* / overlay_screen.* / bitmap.*
# 它们只服务于已删除的 legacy 路径，且仅被 rhiGl.cpp 中一段约 125 行的
# 字符串比较链引用——那段链在 legacy 删除后已成死代码。
# world_point_p3c3.frag 已更名为 point_p3c3.frag：点的圆形裁剪只发生在屏幕上，
# 与渲染空间无关，原名有误导性。

# 被 #include 进上面这些 shader 的片段。它们本身不作为独立 shader 嵌入，
# 但必须声明为构建依赖，否则修改后不会触发重新生成——这正是旧方案的老问题。
set(RENDERX_SHADER_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/rx_push_constants.glsl
)

set(RENDERX_EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/CMake/EmbedShaders.cmake")
set(RENDERX_GENERATED_SHADER_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/shaderBlobs.cpp")

# 列表要作为单个 -D 参数传给脚本。
# 不能直接用分号：add_custom_command 会把 COMMAND 参数里的 ';' 当作参数分隔符，
# 从而把一个 -D 拆成 22 个参数；而转义成 '\;' 后，脚本侧收到的是字面量
# 反斜杠分号，foreach(IN LISTS) 只会迭代一次（整串被当成一个路径）。
# 因此改用 '|' 作为分隔符，由脚本还原成 CMake 列表——路径中不会出现 '|'。
string(REPLACE ";" "|" RENDERX_SHADER_SOURCES_ARG "${RENDERX_SHADER_SOURCES}")

add_custom_command(
    OUTPUT "${RENDERX_GENERATED_SHADER_CPP}"
    COMMAND ${CMAKE_COMMAND}
        "-DEMBED_INPUT_LIST=${RENDERX_SHADER_SOURCES_ARG}"
        "-DEMBED_OUTPUT=${RENDERX_GENERATED_SHADER_CPP}"
        -P "${RENDERX_EMBED_SCRIPT}"
    # 把 shader 文件本身声明为依赖——这正是旧方案缺失的一环：
    # 此前 .vert/.frag 改动不触发任何重新构建
    DEPENDS ${RENDERX_SHADER_SOURCES} ${RENDERX_SHADER_INCLUDES} "${RENDERX_EMBED_SCRIPT}"
    COMMENT "[Renderx] Embedding ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/* into shaderBlobs.cpp"
    VERBATIM
)

# 生成的 shaderBlobs.cpp 与 shaderLibrary.cpp 不直接编进 RenderX，而是单独做成
# OBJECT 库 RenderxShaders（见下方「Shader 对象库」段）。
#
# 原因：add_custom_command 的 OUTPUT 只能被声明它的同一目录中的目标消费。
# Test/ 是子目录，若在那里直接引用 ${RENDERX_GENERATED_SHADER_CPP}，
# Makefile 生成器会报 "no rule to make target"。抽成 OBJECT 库后，
# RenderX 与两个测试目标都通过 target_link_libraries 复用同一份对象文件，
# 既不重复编译，也不会重复定义符号。
list(REMOVE_ITEM RENDER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/shader/shaderLibrary.cpp")

# 让 shader 源文件出现在 IDE 工程树中（不参与编译）
set_source_files_properties(${RENDERX_SHADER_SOURCES} ${RENDERX_SHADER_INCLUDES}
    PROPERTIES HEADER_FILE_ONLY TRUE)


# ==================== 依赖库配置 ====================

# OpenGL库配置（跨平台）
if(WIN32)
    # Windows平台：直接使用系统opengl32.lib
    set(OPENGL_LIBRARIES opengl32)
elseif(UNIX AND NOT APPLE)
    # Linux平台：通过find_package查找OpenGL
    find_package(OpenGL REQUIRED)
    set(OPENGL_LIBRARIES OpenGL::GL)
else()
    # macOS平台：通过find_package查找OpenGL
    find_package(OpenGL REQUIRED)
    set(OPENGL_LIBRARIES OpenGL::GL)
endif()

# Log库配置（作为子目录构建，自动导出 target）
# 注意：顶层 CMakeLists 已 add_subdirectory(Log/Log) 提供全局 Log 目标；
# 这里仅在顶层尚未提供时才作为子目录纳入，避免重复定义 "Log" 目标。
if(NOT TARGET Log AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Log/Log/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Log" "${CMAKE_CURRENT_BINARY_DIR}/Log" EXCLUDE_FROM_ALL)
    message(STATUS "[Renderx] Log library added as subdirectory")
endif()

# ==================== Shader 对象库 ====================
#
# RenderxShaders 承载两个文件：
#   - 构建期生成的 shaderBlobs.cpp（shader 字节数组）
#   - shaderLibrary.cpp（按名字+语言查表）
# 由 RenderX 与 Test/ 下的两个测试目标共同链接，见上方 REMOVE_ITEM 处的说明。
add_library(RenderxShaders OBJECT
    "${RENDERX_GENERATED_SHADER_CPP}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shader/shaderLibrary.cpp"
)

target_compile_features(RenderxShaders PRIVATE cxx_std_17)

# shader 表是 DLL 内部实现，绝不导出；与 RenderX 目标保持同样的 hidden 可见性。
# POSITION_INDEPENDENT_CODE 是必须的：这些对象文件会被链进 SHARED 库。
set_target_properties(RenderxShaders PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    FOLDER "Render"
)

# shaderLibrary.cpp 只需要 src/ 下的头文件。
# 刻意不链接 Log：shader 查表是纯数据逻辑，引入业务日志库会让
# 「RenderX 可脱离本仓库独立复用」重新失效——这是最后一处业务耦合。
target_include_directories(RenderxShaders PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# ==================== 库目标配置 ====================

# 创建渲染库目标（统一入口，显式 SHARED，不依赖 BUILD_SHARED_LIBS 全局选项）
sanyi_add_shared_library(${PROJECT_NAME}
    FOLDER "Render"
    DESCRIPTION "SanYi CAD Render Library (Industrial-Grade 2D/3D Rendering)"
    EXPORT_MACRO "RENDER_EXPORTS"
    SOURCES ${RENDER_SOURCES}
    HEADERS ${RENDER_PUBLIC_HEADERS}
    PUBLIC_INCLUDE_DIRS
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${RENDER_INCLUDE_DIRS}
    COMPILE_DEFINITIONS
        ${RENDER_COMPILE_DEFS}
    COMPILE_OPTIONS
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-conversion;-Wno-float-conversion>
        LINK_LIBRARIES
            PRIVATE
                ${OPENGL_LIBRARIES}
                RenderxShaders
                ${RENDER_LINK_LIBS}
)

# ==================== 符号可见性 ====================
#
# 关键修复：此前全仓库未设置任何 visibility 属性，导致平台行为严重不对称——
# Windows/MSVC 只导出标注了 __declspec(dllexport) 的 C 函数（约 74 个），
# 而 Linux/macOS 默认 -fvisibility=default，把 RenderWorld、GLDevice、
# shader::*、以及编入 DLL 的 stb_truetype 全部内部符号一并导出。
# 后果：(1) 出现「Mac 能编、Windows 链接失败」的偶然依赖；
#       (2) stb 符号 default 可见，可能与宿主/Qt 携带的另一份 stb 串用；
#       (3) 动态符号表膨胀。
# 设为 hidden 后，三平台的导出面统一由 RENDER_API 宏决定。
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# stb_truetype 是**必需**依赖：字形光栅化（src/rt/rxFont.cpp）没有它就编不过。
# 此前这里是静默可选的，配不到就少个 include 目录，报错时呈现为
# 「找不到 stb_truetype.h」而不是「依赖没装」。
find_path(STB_TRUETYPE_INCLUDE_DIR stb_truetype.h QUIET)
if(STB_TRUETYPE_INCLUDE_DIR)
    target_include_directories(${PROJECT_NAME} PRIVATE ${STB_TRUETYPE_INCLUDE_DIR})
elseif(Stb_FOUND)
    # 如果通过find_package找到Stb，使用其路径
    target_include_directories(${PROJECT_NAME} PRIVATE ${Stb_INCLUDE_DIR})
elseif(DEFINED VCPKG_DIR)
    # vcpkg: 头文件位于 <triplet>/include 下（兼容不同平台 triplet）
    file(GLOB _vcpkg_stb_incs "${VCPKG_DIR}/installed/*/include")
    list(REMOVE_DUPLICATES _vcpkg_stb_incs)
    if(_vcpkg_stb_incs)
        target_include_directories(${PROJECT_NAME} PRIVATE ${_vcpkg_stb_incs})
    else()
        message(FATAL_ERROR "未找到 stb_truetype.h（字形光栅化必需）。请执行 vcpkg install stb")
    endif()
else()
    message(FATAL_ERROR "未找到 stb_truetype.h（字形光栅化必需）。请执行 vcpkg install stb")
endif()

# Linux平台下GCC编译器需要链接stdc++fs（用于std::filesystem）
if(NOT WIN32 AND ${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
    target_link_libraries(${PROJECT_NAME}
        PRIVATE
            stdc++fs
    )
endif()

# ==================== 资源文件说明 ====================
#
# shader：已在构建期编入 DLL（见上方 Shader 嵌入段），不再有运行时拷贝与安装。
#
# 字体：DLL 不再从磁盘读取字体。原先 renderCreateDevice 用 std::filesystem
# 从可执行文件路径推导目录、再读 default_screen_font.ttf——这让 DLL 依赖运行
# 目录布局，在 macOS .app bundle 下极易失效。现改为由宿主通过
# rxFontLoad(runtime, fontData, dataSize, pixelHeight) 以内存数据注入
# （见 include/render/renderx.h）。src/res/default_screen_font.ttf 保留在
# 仓库中供宿主取用，但不再由本目标复制或安装。

# ==================== 安装配置 ====================

# 安装库文件
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin    # 动态库安装到bin目录
    LIBRARY DESTINATION lib    # 共享库安装到lib目录
)

# 安装头文件目录
install(DIRECTORY include/render
    DESTINATION include
)

# ==================== 构建信息输出 ====================

message(STATUS "[${PROJECT_NAME}]")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Shared Libraries: ${BUILD_SHARED_LIBS}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Log Library: ${LOG_INCLUDE_DIR}")

# ==================== 测试配置 ====================
find_package(GTest QUIET)
if(GTest_FOUND)
    option(BUILD_RENDERX_TESTS "Build Renderx unit tests" ON)
    if(BUILD_RENDERX_TESTS)
        add_subdirectory(Test)
        message(STATUS "[Renderx] Unit tests enabled (Test/)")
    else()
        message(STATUS "[Renderx] Unit tests disabled by BUILD_RENDERX_TESTS=OFF")
    endif()
else()
    message(STATUS "[Renderx] GTest not found, unit tests skipped. Install GTest to enable testing.")
endif()

message(STATUS "----------------------------------------")

sanyi_export_imported_target(${PROJECT_NAME}
    LIB_NAME "${PROJECT_NAME}"
    PUBLIC_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INTERFACE_LINK_LIBRARIES
        Log::Log
)