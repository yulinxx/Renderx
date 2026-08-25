/**
 * @file shaderLibrary.h
 * @brief 内置 shader 库：按名字 + 目标语言查询编入二进制的 shader
 *
 * 取代原 shaders.h 的两点设计变化：
 *
 * 1. 不再有 18 个 extern const char* 全局变量。
 *    旧方案每加一个 shader 就要改三处（.h 声明、.cpp 定义+加载、
 *    后端里的 if-else 名字映射），且 rhiGl.cpp 中为此维护了一段约 125 行的
 *    字符串比较链。现在改为名字键查表，新增 shader 只需把文件加进
 *    CMake 的 RENDERX_SHADER_SOURCES 列表。
 *
 * 2. 不再有运行时文件 IO。
 *    旧方案在 shader::initialize(shaderDir) 中用 std::ifstream 逐个读
 *    .vert/.frag，目录由 renderCreateDevice 用 std::filesystem 从可执行文件
 *    路径推导。这带来两个已记录在案的实际故障（见 Docs/Mac渲染.md §4）：
 *    macOS .app bundle 下路径推导失败导致视口全黑；shader 文件不是构建依赖，
 *    改了不重新复制，调试时跑的是旧 shader。
 *
 * 同一个 shader 可以有多个语言变体（同名不同扩展名），供不同后端选用：
 *   scene_2d.vert       -> Language::Glsl      (OpenGL)
 *   scene_2d.vert.spv   -> Language::SpirV     (Vulkan)
 *   scene_2d.metallib   -> Language::MetalLib  (Metal)
 * 后端通过 Capabilities::acceptedShaderLanguage 声明自己接受的语言，
 * 本库只做查表，不做任何跨语言转译。
 */
#pragma once

#include <cstdint>

namespace Render::shader
{

    /// shader 字节码语言
    enum class Language : uint8_t
    {
        /// GLSL 文本，NUL 结尾，可直接交给 glShaderSource
        Glsl = 0,
        /// SPIR-V 字流（4 字节对齐）
        SpirV = 1,
        /// Metal Shading Language 文本，NUL 结尾
        Msl = 2,
        /// 预编译 .metallib 二进制
        MetalLib = 3,
    };

    /// 一份 shader 数据。data 指向 DLL 只读段，永久有效，不需要释放。
    struct ShaderBlob
    {
        const void* data = nullptr;
        /// 实际字节数。文本语言不含末尾 NUL（但 data 保证以 NUL 结尾）。
        uint64_t sizeBytes = 0;
        Language language = Language::Glsl;
    };

    /**
     * @brief 按名字与语言查找 shader
     *
     * @param name  文件名，含扩展名，例如 "scene_2d.vert"
     * @param lang  目标语言
     * @param out   输出，可为 nullptr（此时仅做存在性检查）
     * @return 找到返回 true；未找到返回 false 并记录一条 Warn 日志
     */
    bool find(const char* name, Language lang, ShaderBlob* out);

    /**
     * @brief GLSL 便捷访问
     *
     * @return NUL 结尾的 GLSL 源码；未找到返回 nullptr
     */
    const char* glslSource(const char* name);

    /// 已嵌入的 shader 总数（含各语言变体）
    uint32_t count();

    /// 第 index 个 shader 的名字，越界返回 nullptr。用于启动期自检与诊断日志。
    const char* nameAt(uint32_t index);

    /// 第 index 个 shader 的语言
    Language languageAt(uint32_t index);

    const char* languageName(Language lang);

}  // namespace Render::shader
