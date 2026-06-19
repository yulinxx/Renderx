#pragma once

#include "render/render_types.h"
#include "../rhi/rhi_device.h"
#include <vector>
#include <cstdint>
#include <string>

namespace render::core {

class TextAtlas
{
public:
    bool initialize(rhi::IDevice* device);
    void shutdown();

    void loadFont(const void* fontData, size_t dataSize, float pixelHeight = 32.0f);

    void renderText(const TextItemList* texts, const float viewMatrix[9], rhi::IDevice* device);

private:
    struct GlyphInfo
    {
        float u0, v0, u1, v1;
        float x0, y0, x1, y1;
        float xAdvance;
        bool  valid;
    };

    struct CachedGlyph
    {
        uint32_t  codepoint;
        float     fontSize;
        GlyphInfo info;
    };

    struct TextVertex
    {
        float px, py, pz;
        float u, v;
        float cr, cg, cb, ca;
    };

    static_assert(sizeof(TextVertex) == 36, "TextVertex must be 36 bytes");

    rhi::TextureHandle  m_atlasTexture  = rhi::NullHandle;
    rhi::BufferHandle   m_vertexBuffer  = rhi::NullHandle;
    rhi::PipelineHandle m_textPipeline  = rhi::NullHandle;

    std::vector<uint8_t>    m_atlasData;
    std::vector<CachedGlyph> m_glyphCache;

    uint32_t m_atlasWidth      = 2048;
    uint32_t m_atlasHeight     = 2048;
    uint32_t m_atlasCursorX    = 0;
    uint32_t m_atlasCursorY    = 0;
    uint32_t m_atlasLineHeight = 0;

    float m_fontPixelHeight = 32.0f;

    char m_stbFontInfo[512] = {};
    bool m_fontLoaded       = false;

    GlyphInfo getGlyph(uint32_t codepoint, float fontSize);
    bool      rasterizeGlyph(uint32_t codepoint, float fontSize, GlyphInfo* outInfo);
    void      buildTextQuads(const TextItem& item, std::vector<TextVertex>& outQuads, const float viewMatrix[9]);
};

}
