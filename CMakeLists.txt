# 指定CMake最低版本要求
cmake_minimum_required(VERSION 4.3)

# 包含上级目录的配置文件（通常包含全局变量定义）
include(${CMAKE_CURRENT_SOURCE_DIR}/../Config.txt)

# 如果定义了VCPKG_DIR环境变量且路径存在，则使用vcpkg作为包管理器
if(DEFINED VCPKG_DIR AND EXISTS "${VCPKG_DIR}")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake")
endif()

# 项目配置
# - 项目名称: SanYiRender
# - 版本: 2.0.0
# - 描述: 工业级2D/3D渲染库
# - 语言: C++
project(SanYiRender
    VERSION 2.0.0
    DESCRIPTION "SanYi CAD Render Library (Industrial-Grade 2D/3D Rendering)"
    LANGUAGES CXX
)

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

# 构建选项：是否构建共享库（默认开启）
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# ==================== 源文件配置 ====================

# 公开头文件列表（对外暴露的API头文件）
set(RENDER_PUBLIC_HEADERS
    include/render/render.h
    include/render/render_types.h
)

# 源文件列表
set(RENDER_SOURCES
    src/c_api/render_c_api.cpp          # C API实现
    src/platform/gl_loader.cpp          # OpenGL函数加载器
    src/rhi/rhi_gl.cpp                  # OpenGL RHI实现
    src/core/render_world.cpp           # 渲染世界（图元管理）
    src/core/batch_queue.cpp            # 批量绘制队列
    src/core/overlay_queue.cpp          # 叠加层渲染队列
    src/core/transient_buffer_pool.cpp  # 瞬态缓冲区池（Persistent Mapped Buffer）
    src/core/command_encoder.cpp        # 统一命令编码器（Phase 3 新增）
    src/core/render_graph.cpp           # 渲染图 Pass 调度层（Phase 4 新增）
    src/core/pipeline_state_manager.cpp # 管线状态管理器（Phase 7 新增）
    src/core/draw_batcher.cpp           # 绘制合批器（Phase 8 新增）
    src/core/persistent_entity_manager.cpp # 持久图元管理器（Phase 9 新增）
    src/core/mesh_manager.cpp           # 3D网格管理器
    src/core/text_atlas.cpp             # 文本图集管理器
    src/core/scene_env.cpp              # 场景环境渲染器
    src/core/stb_truetype_impl.cpp      # stb_truetype字体库实现
    src/shader/shaders.cpp              # Shader加载管理
)

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

# Log库配置（项目内部日志库）
# 通过 target_link_libraries(Log) 自动获取头文件路径和链接
set(LOG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Log/Log/Include")
if(EXISTS "${LOG_INCLUDE_DIR}/Log/SyLogger.h")
    message(STATUS "[Renderx] Found Log library include: ${LOG_INCLUDE_DIR}")
else()
    message(WARNING "[Renderx] Log library include not found: ${LOG_INCLUDE_DIR}")
endif()

# ==================== 库目标配置 ====================

# 创建渲染库目标
add_library(${PROJECT_NAME} ${RENDER_SOURCES})

set_target_properties(${PROJECT_NAME} PROPERTIES
    DEBUG_POSTFIX "_d"
)

# 设置头文件搜索路径
target_include_directories(${PROJECT_NAME}
    PUBLIC
        # 构建时的公开头文件路径
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        # 私有头文件路径（内部实现使用）
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 编译定义
target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        # 导出符号定义（用于DLL导出）
        RENDER_EXPORTS
)

# 链接依赖库
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        # OpenGL库
        ${OPENGL_LIBRARIES}
        # Log库（项目内部日志库）
        Log
)

# stb库头文件配置（用于文本渲染）
if(Stb_FOUND)
    # 如果通过find_package找到Stb，使用其路径
    target_include_directories(${PROJECT_NAME} PRIVATE ${Stb_INCLUDE_DIR})
elseif(DEFINED VCPKG_DIR AND EXISTS "${VCPKG_DIR}/installed/x64-windows/include")
    # 如果使用vcpkg，从vcpkg安装目录查找
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${VCPKG_DIR}/installed/x64-windows/include")
endif()

# Linux平台下GCC编译器需要链接stdc++fs（用于std::filesystem）
if(NOT WIN32 AND ${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
    target_link_libraries(${PROJECT_NAME}
        PRIVATE
            stdc++fs
    )
endif()

# ==================== Shader文件配置 ====================

# 所有Shader文件列表
set(SHADER_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/scene_2d.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/scene_2d.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/overlay.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/overlay.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/overlay_screen.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/overlay_screen.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/bitmap.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/bitmap.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/mesh_3d.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/mesh_3d.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/mesh_3d_instanced.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/text_sdf.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/text_sdf.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/highlight_3d.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/highlight_3d.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/culling.comp
)

# 构建后自动复制Shader文件到输出目录
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SHADER_FILES}
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
    COMMENT "Copying shader files to output directory"
)

# ==================== 安装配置 ====================

# 安装库文件
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin    # 动态库安装到bin目录
    LIBRARY DESTINATION lib    # 共享库安装到lib目录
    ARCHIVE DESTINATION lib    # 静态库安装到lib目录
)

# 安装头文件目录
install(DIRECTORY include/render
    DESTINATION include
)

# 安装Shader文件
install(FILES ${SHADER_FILES}
    DESTINATION bin
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