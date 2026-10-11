/**
 * @file RawShaderBlob.h
 * @brief 生成式 shader 条目结构体（供 shaderBlobs.cpp 与 shaderLibrary.cpp 共用）
 *
 * 由 CMake/EmbedShaders.cmake 生成的 shaderBlobs.cpp 与手写的 shaderLibrary.cpp
 * 共同依赖此定义，避免循环依赖与前向声明不足的问题。
 */
#pragma once

#include <cstdint>

namespace Render
{
    namespace shader
    {
        namespace generated
        {
            /// 单条嵌入式 shader 条目
            struct RawShaderBlob
            {
                const char* name;
                const char* language;
                const unsigned char* data;
                unsigned long long sizeBytes;
            };

        }  // namespace generated
    }  // namespace shader
}  // namespace Render