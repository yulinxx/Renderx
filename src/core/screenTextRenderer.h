/**
 * @file screen_text_renderer.h
 * @brief 屏幕空间文本渲染器
 *
 * 通用屏幕空间文本渲染组件，使用 stb_truetype 栅格化字体到纹理图集，
 * 以像素坐标提交文本四边形，不随视图缩放变化大小。
 * 适用于标尺刻度、图元序号标识等固定像素大小的文字显示。
 */
#pragma once

#include "../rhi/rhi_device.h"
#include "render/render_types.h"
#include <vector>
#include <cstdint>
#include <string>

namespace render::core
{

    class ScreenTextRenderer
    {
    public:
        // 直接复用公共类型，避免重复定义
        using ScreenTextItem = render::ScreenTextItem;

        bool initialize(rhi::IDevice* device);
        void shutdown();
        void loadFont(const void* fontData, size_t dataSize, float pixelHeight = 16.0f);
        void beginFrame();
        void submitText(const ScreenTextItem& item);
        void render(rhi::IDevice* device, uint32_t viewportWidth, uint32_t viewportHeight);
        void clear();

    private:
        struct GlyphInfo
        {
            float u0, v0, u1, v1;  // 纹理 UV（归一化）
            float x0, y0, x1, y1;  // 字符边界（像素）
            float xAdvance;
            bool valid;
        };

        struct CachedGlyph
        {
            uint32_t codepoint;
            float fontSize;
            GlyphInfo info;
        };

        // 2D 像素位置 + 2D 纹理坐标 + 4D 颜色 = 32 字节
        struct TextVertex
        {
            float px, py;          // 像素坐标
            float u, v;            // 纹理坐标
            float cr, cg, cb, ca;  // 颜色
        };

        static_assert(sizeof(TextVertex) == 32, "TextVertex must be 32 bytes");

        rhi::IDevice* m_device = nullptr;
        rhi::TextureHandle m_atlasTexture = rhi::NullHandle;
        rhi::BufferHandle m_vertexBuffer = rhi::NullHandle;
        uint32_t m_vertexBufferCapacity = 0;  // VB 当前分配容量（字节），用于增量上传判断
        rhi::PipelineHandle m_pipeline = rhi::NullHandle;

        std::vector<uint8_t> m_atlasData;
        std::vector<CachedGlyph> m_glyphCache;
        std::vector<TextVertex> m_frameVerts;

        uint32_t m_atlasWidth = 1024;
        uint32_t m_atlasHeight = 1024;
        uint32_t m_atlasCursorX = 0;
        uint32_t m_atlasCursorY = 0;
        uint32_t m_atlasLineHeight = 0;
        float m_fontPixelHeight = 16.0f;
        char m_stbFontInfo[512] = {};
        bool m_fontLoaded = false;
        std::vector<uint8_t> m_fontData;  // 保存字体数据副本，避免悬垂指针
        bool m_atlasDirty = false;

        GlyphInfo getGlyph(uint32_t codepoint, float fontSize);
        bool rasterizeGlyph(uint32_t codepoint, float fontSize, GlyphInfo* outInfo);
    };

}  // namespace render::core
