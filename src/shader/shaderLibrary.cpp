/**
 * @file shaderLibrary.cpp
 * @brief 内置 shader 库实现
 *
 * 数据来自 CMake 生成的 shaderBlobs.cpp（见 CMake/EmbedShaders.cmake）。
 * 本文件只做名字 + 语言的线性查表——shader 总数是十几个量级，
 * 且查表只发生在管线创建时（非绘制热路径），不需要哈希表。
 */
#include "shader/shaderLibrary.h"

#include "Log/SyLogger.h"

#include <cstring>

namespace Render::shader
{
    namespace generated
    {
        /// 与 CMake/EmbedShaders.cmake 生成的结构保持一致。
        /// 故意用内建类型而非 <cstdint>，使生成文件无需包含任何头文件。
        struct RawShaderBlob
        {
            const char* name;
            const char* language;
            const unsigned char* data;
            unsigned long long sizeBytes;
        };

        extern const RawShaderBlob kShaderBlobs[];
        extern const unsigned long long kShaderBlobCount;
    }  // namespace generated

    namespace
    {
        /// 生成侧用字符串标签表示语言（避免生成文件依赖本头的枚举），
        /// 在此转回强类型枚举。
        bool parseLanguage(const char* tag, Language* out)
        {
            if (std::strcmp(tag, "glsl") == 0)
            {
                *out = Language::Glsl;
                return true;
            }
            if (std::strcmp(tag, "spirv") == 0)
            {
                *out = Language::SpirV;
                return true;
            }
            if (std::strcmp(tag, "msl") == 0)
            {
                *out = Language::Msl;
                return true;
            }
            if (std::strcmp(tag, "metallib") == 0)
            {
                *out = Language::MetalLib;
                return true;
            }
            return false;
        }
    }  // namespace

    const char* languageName(Language lang)
    {
        switch (lang)
        {
        case Language::Glsl: return "glsl";
        case Language::SpirV: return "spirv";
        case Language::Msl: return "msl";
        case Language::MetalLib: return "metallib";
        }
        return "unknown";
    }

    bool find(const char* name, Language lang, ShaderBlob* out)
    {
        if (!name)
        {
            SY_WARNF("[shaderLibrary] find called with null name");
            return false;
        }

        for (unsigned long long i = 0; i < generated::kShaderBlobCount; ++i)
        {
            const generated::RawShaderBlob& raw = generated::kShaderBlobs[i];

            if (std::strcmp(raw.name, name) != 0)
                continue;

            Language rawLang{};
            if (!parseLanguage(raw.language, &rawLang))
            {
                // 生成器与本文件的语言标签不一致，属于构建配置错误而非运行时输入错误
                SY_ERRORF("[shaderLibrary] unknown language tag '%s' on shader '%s'",
                          raw.language, raw.name);
                continue;
            }
            if (rawLang != lang)
                continue;

            if (out)
            {
                out->data = raw.data;
                out->sizeBytes = raw.sizeBytes;
                out->language = rawLang;
            }
            return true;
        }

        SY_WARNF("[shaderLibrary] shader not found: name='%s' language=%s (%llu blobs embedded)",
                 name, languageName(lang),
                 static_cast<unsigned long long>(generated::kShaderBlobCount));
        return false;
    }

    const char* glslSource(const char* name)
    {
        ShaderBlob blob{};
        if (!find(name, Language::Glsl, &blob))
            return nullptr;

        // 生成器为文本类语言在数据末尾补了 NUL，可直接当 C 字符串使用
        return static_cast<const char*>(blob.data);
    }

    uint32_t count()
    {
        return static_cast<uint32_t>(generated::kShaderBlobCount);
    }

    const char* nameAt(uint32_t index)
    {
        if (index >= generated::kShaderBlobCount)
            return nullptr;
        return generated::kShaderBlobs[index].name;
    }

    Language languageAt(uint32_t index)
    {
        if (index >= generated::kShaderBlobCount)
            return Language::Glsl;

        Language lang{};
        if (!parseLanguage(generated::kShaderBlobs[index].language, &lang))
            return Language::Glsl;
        return lang;
    }

}  // namespace Render::shader
