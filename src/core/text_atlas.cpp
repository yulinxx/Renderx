#include "text_atlas.h"
#include "shader/shaders.h"

#include <stb_truetype.h>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace render::core
{
    bool TextAtlas::initialize(rhi::IDevice* device)
    {
        m_device = device;

        m_atlasData.resize(m_atlasWidth * m_atlasHeight * 4, 0);

        {
            rhi::TextureDesc desc{};
            desc.width = m_atlasWidth;
            desc.height = m_atlasHeight;
            desc.format = rhi::Format::RGBA8;
            desc.mipLevels = 1;
            desc.debugName = "TextAtlas";
            m_atlasTexture = device->createTexture(desc);
            if (m_atlasTexture == rhi::NullHandle)
                return false;

            device->uploadTexture(m_atlasTexture, 0, m_atlasData.data(), m_atlasWidth * 4);
        }

        {
            rhi::BufferDesc desc{};
            desc.size = 64 * 1024;
            desc.usage = rhi::BufferUsage::Vertex;
            desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
            desc.debugName = "TextAtlasVB";
            m_vertexBuffer = device->createBuffer(desc);
            if (m_vertexBuffer == rhi::NullHandle)
                return false;
        }

        {
            rhi::PipelineDesc desc{};
            desc.topology = rhi::PrimitiveTopology::TriangleList;
            desc.vertexShader = shader::TEXT_SDF_VERT;
            desc.fragmentShader = shader::TEXT_SDF_FRAG;
            desc.computeShader = nullptr;
            desc.vertexFormat = rhi::VertexFormat::P3T2C4;
            desc.depthTest = false;
            desc.depthWrite = false;
            desc.blendEnable = true;
            desc.srcBlend = rhi::BlendFactor::SrcAlpha;
            desc.dstBlend = rhi::BlendFactor::OneMinusSrcAlpha;
            desc.depthFunc = rhi::CompareFunc::Always;
            m_textPipeline = device->createPipeline(desc);
            if (m_textPipeline == rhi::NullHandle)
                return false;
        }

        return true;
    }

    void TextAtlas::shutdown()
    {
        if (m_device)
        {
            if (m_atlasTexture != rhi::NullHandle)
            {
                m_device->destroyTexture(m_atlasTexture);
                m_atlasTexture = rhi::NullHandle;
            }
            if (m_vertexBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_vertexBuffer);
                m_vertexBuffer = rhi::NullHandle;
            }
            if (m_textPipeline != rhi::NullHandle)
            {
                m_device->destroyPipeline(m_textPipeline);
                m_textPipeline = rhi::NullHandle;
            }
        }

        m_atlasData.clear();
        m_glyphCache.clear();
        m_atlasCursorX = 0;
        m_atlasCursorY = 0;
        m_atlasLineHeight = 0;
        m_fontLoaded = false;
        m_device = nullptr;
    }

    void TextAtlas::loadFont(const void* fontData, size_t /*dataSize*/, float pixelHeight)
    {
        stbtt_fontinfo fi;
        if (!stbtt_InitFont(&fi, static_cast<const unsigned char*>(fontData), 0))
            return;

        memcpy(m_stbFontInfo, &fi, sizeof(fi));
        m_fontPixelHeight = pixelHeight;
        m_fontLoaded = true;
        m_glyphCache.clear();
        m_atlasCursorX = 0;
        m_atlasCursorY = 0;
        m_atlasLineHeight = 0;
    }

    void TextAtlas::renderText(const TextItemList* texts, const float viewMatrix[9],
        uint32_t viewportW, uint32_t viewportH,
        rhi::IDevice* device)
    {
        if (!texts || !texts->items || texts->count == 0 || !m_fontLoaded)
            return;
        if (viewportW == 0 || viewportH == 0)
            return;

        std::vector<TextVertex> allQuads;
        allQuads.reserve(texts->count * 64);

        for (uint32_t i = 0; i < texts->count; ++i)
        {
            const TextItem& item = texts->items[i];
            if (!item.text || item.text[0] == '\0')
                continue;
            buildTextQuads(item, allQuads, viewMatrix, viewportW, viewportH);
        }

        if (allQuads.empty())
            return;

        uint32_t vertBytes = static_cast<uint32_t>(allQuads.size() * sizeof(TextVertex));
        // 扩容
        rhi::BufferDesc vbDesc;
        vbDesc.size = vertBytes;
        vbDesc.usage = rhi::BufferUsage::Vertex;
        vbDesc.memory = rhi::MemoryType::GPU_CPU_Coherent;
        vbDesc.debugName = "TextAtlasVB_Resize";
        if (m_vertexBuffer != rhi::NullHandle)
            device->destroyBuffer(m_vertexBuffer);
        m_vertexBuffer = device->createBuffer(vbDesc);

        device->uploadBuffer(m_vertexBuffer, 0, vertBytes, allQuads.data());

        // 上传图集纹理（如有脏数据）
        device->uploadTexture(m_atlasTexture, 0, m_atlasData.data(), m_atlasWidth * 4);

        // 渲染：顶点已是NDC坐标，shader中不再需要viewMatrix变换
        device->bindPipeline(m_textPipeline);
        device->bindVertexBuffer(0, m_vertexBuffer, 0);
        device->bindTexture(0, 0, m_atlasTexture);
        device->draw(static_cast<uint32_t>(allQuads.size()), 1, 0, 0);
    }

    TextAtlas::GlyphInfo TextAtlas::getGlyph(uint32_t codepoint, float fontSize)
    {
        for (auto& cg : m_glyphCache)
        {
            if (cg.codepoint == codepoint && cg.fontSize == fontSize)
                return cg.info;
        }

        GlyphInfo info{};
        if (rasterizeGlyph(codepoint, fontSize, &info))
        {
            m_glyphCache.push_back({ codepoint, fontSize, info });
        }
        return info;
    }

    bool TextAtlas::rasterizeGlyph(uint32_t codepoint, float fontSize, GlyphInfo* outInfo)
    {
        if (!m_fontLoaded)
            return false;

        stbtt_fontinfo fi;
        memcpy(&fi, m_stbFontInfo, sizeof(fi));

        float scale = stbtt_ScaleForPixelHeight(&fi, fontSize);

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&fi, static_cast<int>(codepoint), scale, scale, &x0, &y0, &x1, &y1);

        int bmpW = x1 - x0;
        int bmpH = y1 - y0;

        int advance, lsb;
        stbtt_GetCodepointHMetrics(&fi, static_cast<int>(codepoint), &advance, &lsb);

        if (bmpW <= 0 || bmpH <= 0)
        {
            outInfo->u0 = 0; outInfo->v0 = 0;
            outInfo->u1 = 0; outInfo->v1 = 0;
            outInfo->x0 = 0; outInfo->y0 = 0;
            outInfo->x1 = 0; outInfo->y1 = 0;
            outInfo->xAdvance = advance * scale;
            outInfo->valid = true;
            return true;
        }

        if (m_atlasCursorX + bmpW > m_atlasWidth)
        {
            m_atlasCursorX = 0;
            m_atlasCursorY += m_atlasLineHeight;
            m_atlasLineHeight = 0;
        }

        if (m_atlasCursorY + bmpH > m_atlasHeight)
            return false;

        std::vector<uint8_t> bitmap(bmpW * bmpH);
        stbtt_MakeCodepointBitmap(&fi, bitmap.data(), bmpW, bmpH, bmpW, scale, scale, static_cast<int>(codepoint));

        uint32_t dstX = m_atlasCursorX;
        uint32_t dstY = m_atlasCursorY;

        for (int row = 0; row < bmpH; ++row)
        {
            for (int col = 0; col < bmpW; ++col)
            {
                uint32_t srcIdx = row * bmpW + col;
                uint32_t dstIdx = ((dstY + row) * m_atlasWidth + (dstX + col)) * 4;
                uint8_t  val = bitmap[srcIdx];
                m_atlasData[dstIdx + 0] = val;
                m_atlasData[dstIdx + 1] = val;
                m_atlasData[dstIdx + 2] = val;
                m_atlasData[dstIdx + 3] = val;
            }
        }

        float invW = 1.0f / static_cast<float>(m_atlasWidth);
        float invH = 1.0f / static_cast<float>(m_atlasHeight);

        outInfo->u0 = static_cast<float>(dstX) * invW;
        outInfo->v0 = static_cast<float>(dstY) * invH;
        outInfo->u1 = static_cast<float>(dstX + bmpW) * invW;
        outInfo->v1 = static_cast<float>(dstY + bmpH) * invH;

        outInfo->x0 = static_cast<float>(x0);
        outInfo->y0 = static_cast<float>(y0);
        outInfo->x1 = static_cast<float>(x1);
        outInfo->y1 = static_cast<float>(y1);

        outInfo->xAdvance = advance * scale;
        outInfo->valid = true;

        m_atlasCursorX += bmpW;
        m_atlasLineHeight = (std::max)(m_atlasLineHeight, static_cast<uint32_t>(bmpH));

        return true;
    }

    void TextAtlas::buildTextQuads(const TextItem& item, std::vector<TextVertex>& outQuads,
        const float viewMatrix[9],
        uint32_t viewportW, uint32_t viewportH)
    {
        // 用 viewMatrix 将文本基点从世界坐标变换到 NDC
        float wx = item.x;
        float wy = item.y;
        float ndcX = viewMatrix[0] * wx + viewMatrix[3] * wy + viewMatrix[6];
        float ndcY = viewMatrix[1] * wx + viewMatrix[4] * wy + viewMatrix[7];

        // 像素到 NDC 的转换系数
        float pxToNdcX = 2.0f / static_cast<float>(viewportW);
        float pxToNdcY = 2.0f / static_cast<float>(viewportH);

        float fontSize = static_cast<float>(item.fontSize);
        if (fontSize <= 0.0f)
            fontSize = m_fontPixelHeight;

        // 第一遍：计算文本总宽度和高度，用于对齐
        float totalWidth = 0.0f;
        float ascent = 0.0f;
        float descent = 0.0f;
        for (const char* p = item.text; *p != '\0'; ++p)
        {
            uint32_t cp = static_cast<uint32_t>(static_cast<unsigned char>(*p));
            GlyphInfo gi = getGlyph(cp, fontSize);
            if (!gi.valid) continue;
            totalWidth += gi.xAdvance;
            if (gi.y0 < descent) descent = gi.y0;
            if (gi.y1 > ascent) ascent = gi.y1;
        }
        float totalHeight = ascent - descent;

        // 根据对齐方式计算偏移
        float alignOffsetX = 0.0f;
        float alignOffsetY = 0.0f;
        if (item.hAlign == 1)      // Center
            alignOffsetX = -totalWidth * 0.5f;
        else if (item.hAlign == 2) // Right
            alignOffsetX = -totalWidth;

        if (item.vAlign == 1)       // Top
            alignOffsetY = -ascent;
        else if (item.vAlign == 2)  // Middle
            alignOffsetY = -totalHeight * 0.5f - descent;
        else if (item.vAlign == 3)  // Bottom
            alignOffsetY = -descent;

        // 旋转参数
        float rotRad = item.rotationDeg * 3.14159265358979323846f / 180.0f;
        float cosR = std::cos(rotRad);
        float sinR = std::sin(rotRad);

        // 第二遍：构建四边形
        float cursorX = 0.0f;
        for (const char* p = item.text; *p != '\0'; ++p)
        {
            uint32_t cp = static_cast<uint32_t>(static_cast<unsigned char>(*p));
            GlyphInfo gi = getGlyph(cp, fontSize);
            if (!gi.valid)
                continue;

            // 字符的像素偏移（含对齐偏移）
            float px = cursorX + gi.x0 + alignOffsetX;
            float py = gi.y0 + alignOffsetY;

            // 像素偏移转 NDC
            float qx = px * pxToNdcX;
            float qy = py * pxToNdcY;
            float qw = (gi.x1 - gi.x0) * pxToNdcX;
            float qh = (gi.y1 - gi.y0) * pxToNdcY;

            // 如果有旋转，旋转字符的 NDC 偏移
            float rx0, ry0, rx1, ry1, rx2, ry2, rx3, ry3;
            if (std::fabs(rotRad) > 1e-6f)
            {
                // 四个角的偏移
                float ox[4] = { qx, qx + qw, qx + qw, qx };
                float oy[4] = { qy, qy, qy + qh, qy + qh };
                for (int k = 0; k < 4; ++k)
                {
                    float rx = ox[k] * cosR - oy[k] * sinR;
                    float ry = ox[k] * sinR + oy[k] * cosR;
                    ox[k] = rx; oy[k] = ry;
                }
                rx0 = ox[0]; ry0 = oy[0];
                rx1 = ox[1]; ry1 = oy[1];
                rx2 = ox[2]; ry2 = oy[2];
                rx3 = ox[3]; ry3 = oy[3];
            }
            else
            {
                rx0 = qx;       ry0 = qy;
                rx1 = qx + qw;  ry1 = qy;
                rx2 = qx + qw;  ry2 = qy + qh;
                rx3 = qx;       ry3 = qy + qh;
            }

            // 最终顶点位置 = 基点NDC + 旋转后的字符NDC偏移
            TextVertex v0 = { ndcX + rx0, ndcY + ry0, 0.0f, gi.u0, gi.v0, item.color[0], item.color[1], item.color[2], item.color[3] };
            TextVertex v1 = { ndcX + rx1, ndcY + ry1, 0.0f, gi.u1, gi.v0, item.color[0], item.color[1], item.color[2], item.color[3] };
            TextVertex v2 = { ndcX + rx2, ndcY + ry2, 0.0f, gi.u1, gi.v1, item.color[0], item.color[1], item.color[2], item.color[3] };
            TextVertex v3 = { ndcX + rx3, ndcY + ry3, 0.0f, gi.u0, gi.v1, item.color[0], item.color[1], item.color[2], item.color[3] };

            outQuads.push_back(v0);
            outQuads.push_back(v1);
            outQuads.push_back(v2);
            outQuads.push_back(v0);
            outQuads.push_back(v2);
            outQuads.push_back(v3);

            cursorX += gi.xAdvance;
        }
    }
}