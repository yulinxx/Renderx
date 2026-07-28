#include "screen_text_renderer.h"
#include "shader/shaders.h"
#include <stb_truetype.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include "Log/SyLogger.h"

namespace render::core
{
    bool ScreenTextRenderer::initialize(rhi::IDevice* device)
    {
        m_device = device;
        m_atlasData.resize(m_atlasWidth * m_atlasHeight * 4, 0);

        // 图集纹理
        {
            rhi::TextureDesc desc{};
            desc.width = m_atlasWidth;
            desc.height = m_atlasHeight;
            desc.format = rhi::Format::RGBA8;
            desc.mipLevels = 1;
            desc.debugName = "ScreenTextAtlas";
            m_atlasTexture = device->createTexture(desc);
            if (m_atlasTexture == rhi::NullHandle) return false;
            device->uploadTexture(m_atlasTexture, 0, m_atlasData.data(), m_atlasWidth * 4);
        }

        // 顶点缓冲
        {
            rhi::BufferDesc desc{};
            desc.size = 64 * 1024;
            desc.usage = rhi::BufferUsage::Vertex;
            desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
            desc.debugName = "ScreenTextVB";
            m_vertexBuffer = device->createBuffer(desc);
            if (m_vertexBuffer == rhi::NullHandle) return false;
        }

        // 屏幕空间文本管线（P2T2C4 顶点格式）
        {
            rhi::PipelineDesc desc{};
            desc.topology = rhi::PrimitiveTopology::TriangleList;
            desc.vertexShader = shader::TEXT_SCREEN_VERT;
            desc.fragmentShader = shader::TEXT_SCREEN_FRAG;
            desc.computeShader = nullptr;
            desc.vertexFormat = rhi::VertexFormat::P2T2C4;
            desc.depthTest = false;
            desc.depthWrite = false;
            desc.blendEnable = true;
            desc.srcBlend = rhi::BlendFactor::SrcAlpha;
            desc.dstBlend = rhi::BlendFactor::OneMinusSrcAlpha;
            desc.depthFunc = rhi::CompareFunc::Always;
            m_pipeline = device->createPipeline(desc);
            if (m_pipeline == rhi::NullHandle)
            {
                SY_ERROR("ScreenTextRenderer::initialize: pipeline creation failed");
                return false;
            }
        }

        SY_INFO("ScreenTextRenderer::initialize: done");
        return true;
    }

    void ScreenTextRenderer::shutdown()
    {
        if (m_device)
        {
            if (m_atlasTexture != rhi::NullHandle)
            {
                m_device->destroyTexture(m_atlasTexture); m_atlasTexture = rhi::NullHandle;
            }
            if (m_vertexBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_vertexBuffer); m_vertexBuffer = rhi::NullHandle;
            }
            if (m_pipeline != rhi::NullHandle)
            {
                m_device->destroyPipeline(m_pipeline); m_pipeline = rhi::NullHandle;
            }
        }
        m_atlasData.clear();
        m_glyphCache.clear();
        m_frameVerts.clear();
        m_atlasCursorX = 0;
        m_atlasCursorY = 0;
        m_atlasLineHeight = 0;
        m_fontLoaded = false;
        m_atlasDirty = false;
        m_device = nullptr;
    }

    void ScreenTextRenderer::loadFont(const void* fontData, size_t dataSize, float pixelHeight)
    {
        // 保存字体数据副本，stbtt_fontinfo 内部持有原始数据指针
        m_fontData.assign(static_cast<const uint8_t*>(fontData),
                          static_cast<const uint8_t*>(fontData) + dataSize);
        stbtt_fontinfo fi;
        if (!stbtt_InitFont(&fi, m_fontData.data(), 0))
            return;
        memcpy(m_stbFontInfo, &fi, sizeof(fi));
        m_fontPixelHeight = pixelHeight;
        m_fontLoaded = true;
        m_glyphCache.clear();
        m_atlasCursorX = 0;
        m_atlasCursorY = 0;
        m_atlasLineHeight = 0;
        // 清空图集
        std::fill(m_atlasData.begin(), m_atlasData.end(), uint8_t(0));
        m_atlasDirty = true;
        SY_INFOF("ScreenTextRenderer::loadFont: pixelHeight=%.1f", pixelHeight);
    }

    void ScreenTextRenderer::beginFrame()
    {
        m_frameVerts.clear();
    }

    void ScreenTextRenderer::submitText(const ScreenTextItem& item)
    {
        if (!m_fontLoaded || !item.text || item.text[0] == '\0') return;

        float fontSize = item.fontSize > 0.0f ? item.fontSize : m_fontPixelHeight;
        float cursorX = item.x;
        float cursorY = item.y;

        for (const char* p = item.text; *p != '\0'; ++p)
        {
            uint32_t cp = static_cast<uint32_t>(static_cast<unsigned char>(*p));
            GlyphInfo gi = getGlyph(cp, fontSize);
            if (!gi.valid) continue;

            float qx = cursorX + gi.x0;
            float qy = cursorY + gi.y0;
            float qw = gi.x1 - gi.x0;
            float qh = gi.y1 - gi.y0;

            // 两个三角形组成四边形
            TextVertex v0 = { qx,      qy,      gi.u0, gi.v1, item.color[0], item.color[1], item.color[2], item.color[3] };
            TextVertex v1 = { qx + qw, qy,      gi.u1, gi.v1, item.color[0], item.color[1], item.color[2], item.color[3] };
            TextVertex v2 = { qx + qw, qy + qh, gi.u1, gi.v0, item.color[0], item.color[1], item.color[2], item.color[3] };
            TextVertex v3 = { qx,      qy + qh, gi.u0, gi.v0, item.color[0], item.color[1], item.color[2], item.color[3] };

            m_frameVerts.push_back(v0);
            m_frameVerts.push_back(v1);
            m_frameVerts.push_back(v2);
            m_frameVerts.push_back(v0);
            m_frameVerts.push_back(v2);
            m_frameVerts.push_back(v3);

            cursorX += gi.xAdvance;
        }
    }

    void ScreenTextRenderer::render(rhi::IDevice* device, uint32_t viewportWidth, uint32_t viewportHeight)
    {
        if (m_frameVerts.empty()) return;

        // 确保 VB 容量够
        uint32_t vertBytes = static_cast<uint32_t>(m_frameVerts.size() * sizeof(TextVertex));
        if (m_vertexBuffer != rhi::NullHandle)
            device->destroyBuffer(m_vertexBuffer);
        rhi::BufferDesc vbDesc{};
        vbDesc.size = vertBytes;
        vbDesc.usage = rhi::BufferUsage::Vertex;
        vbDesc.memory = rhi::MemoryType::GPU_CPU_Coherent;
        vbDesc.debugName = "ScreenTextVB_dyn";
        m_vertexBuffer = device->createBuffer(vbDesc);
        device->uploadBuffer(m_vertexBuffer, 0, vertBytes, m_frameVerts.data());

        // 图集纹理有新字形时上传
        if (m_atlasDirty)
        {
            device->uploadTexture(m_atlasTexture, 0, m_atlasData.data(), m_atlasWidth * 4);
            m_atlasDirty = false;
        }

        // 渲染
        device->bindPipeline(m_pipeline);
        float vpSize[2] = { static_cast<float>(viewportWidth), static_cast<float>(viewportHeight) };
        device->setUniformVec2("uViewportSize", vpSize);
        device->bindVertexBuffer(0, m_vertexBuffer, 0);
        device->bindTexture(0, 0, m_atlasTexture);
        device->draw(static_cast<uint32_t>(m_frameVerts.size()), 1, 0, 0);
    }

    void ScreenTextRenderer::clear()
    {
        m_frameVerts.clear();
    }

    ScreenTextRenderer::GlyphInfo ScreenTextRenderer::getGlyph(uint32_t codepoint, float fontSize)
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

    bool ScreenTextRenderer::rasterizeGlyph(uint32_t codepoint, float fontSize, GlyphInfo* outInfo)
    {
        if (!m_fontLoaded) return false;
        stbtt_fontinfo fi;
        memcpy(&fi, m_stbFontInfo, sizeof(fi));

        float scale = stbtt_ScaleForPixelHeight(&fi, fontSize);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&fi, static_cast<int>(codepoint), scale, scale, &x0, &y0, &x1, &y1);

        int bmpW = x1 - x0;
        int bmpH = y1 - y0;
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&fi, static_cast<int>(codepoint), &advance, &lsb);

        // 空白字符（无像素）
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

        // 行满换行
        if (m_atlasCursorX + bmpW > m_atlasWidth)
        {
            m_atlasCursorX = 0;
            m_atlasCursorY += m_atlasLineHeight;
            m_atlasLineHeight = 0;
        }

        // 图集溢出
        if (m_atlasCursorY + bmpH > m_atlasHeight)
            return false;

        // 栅格化并写入图集
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
                uint8_t val = bitmap[srcIdx];
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
        m_atlasDirty = true;
        return true;
    }
} // namespace render::core