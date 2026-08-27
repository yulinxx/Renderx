/**
 * @file rxFont.cpp
 * @brief Font 的实现：光栅化、图集打包、脏行上传
 *
 * STB_TRUETYPE_IMPLEMENTATION 就定义在这里，不再像旧实现那样单开一个
 * stbTruetypeImpl.cpp——本文件是全 DLL 唯一使用 stb_truetype 的翻译单元，
 * 拆成两个文件只会让「实现在哪」多绕一跳。
 */
#define STB_TRUETYPE_IMPLEMENTATION
// stb_truetype 默认用 assert，且在 NDEBUG 下会退化为空语句；这里显式包含
// <cassert> 以免依赖它自己的兜底定义顺序。
#include <cassert>

#include "rt/rxFont.h"

#include <algorithm>
#include <cstring>

namespace Render::RT::detail
{
    namespace
    {
        /// 未指定时的图集边长。1024x1024 的 R8 是 1MB，够放数百个字形；
        /// 旧实现固定 2048x2048 且是 RGBA8（16MB），对标尺数字是极大浪费。
        constexpr uint32_t kDefaultAtlasSide = 1024;

        /// 字形之间留 1 像素空隙：双线性采样会在图集里跨过字形边界取到
        /// 邻居的像素，表现为字符边缘出现别的字形的残影。
        constexpr uint32_t kGlyphPadding = 1;
    }  // namespace

    RxResult Runtime::createFont(const FontDesc& desc, FontHandle* outFont)
    {
        if (!outFont)
        {
            return RxResult::ErrorInvalidArgument;
        }
        *outFont = FontHandle::Invalid;

        if (!device || !desc.data || desc.dataBytes == 0 || desc.pixelHeight <= 0.0f)
        {
            log.error("[rt] rxFontCreate: 字体数据为空或 pixelHeight 非正");
            return RxResult::ErrorInvalidArgument;
        }

        const uint32_t width = desc.atlasWidth != 0 ? desc.atlasWidth : kDefaultAtlasSide;
        const uint32_t height = desc.atlasHeight != 0 ? desc.atlasHeight : kDefaultAtlasSide;
        if (caps.maxTextureSize != 0 && (width > caps.maxTextureSize || height > caps.maxTextureSize))
        {
            log.error("[rt] rxFontCreate: 图集 %ux%u 超出后端上限 %u", width, height,
                      caps.maxTextureSize);
            return RxResult::ErrorInvalidArgument;
        }

        auto font = new Font();
        font->runtime = this;
        font->data.assign(static_cast<const uint8_t*>(desc.data),
                          static_cast<const uint8_t*>(desc.data) + desc.dataBytes);

        // offset 取 0 号字体：ttc 集合里的其余字体需要调用方自己拆，
        // DLL 不做字体集合解析（那属于字体管理，不是渲染）。
        if (stbtt_InitFont(&font->info, font->data.data(), 0) == 0)
        {
            log.error("[rt] rxFontCreate: 字体数据无法解析（不是 TTF/OTF？）");
            delete font;
            return RxResult::ErrorInvalidArgument;
        }

        font->scale = stbtt_ScaleForPixelHeight(&font->info, desc.pixelHeight);

        int ascent = 0;
        int descent = 0;
        int lineGap = 0;
        stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &lineGap);
        font->metrics.ascent = static_cast<float>(ascent) * font->scale;
        font->metrics.descent = static_cast<float>(descent) * font->scale;
        font->metrics.lineGap = static_cast<float>(lineGap) * font->scale;
        font->metrics.pixelHeight = desc.pixelHeight;

        font->atlasWidth = width;
        font->atlasHeight = height;
        font->sdfPadding = desc.sdfPadding;
        // 清零 = 全透明。未写过的区域被采样到时应当完全不可见。
        //
        // SDF 模式下 0 同样是安全的默认：0 距离轮廓最远（在字形外侧），
        // 片元里 d - 0.5 为负 → alpha 0。若这里填 128（"恰在轮廓上"），
        // 未写过的区域会变成半透明的糊块。
        font->pixels.assign(static_cast<size_t>(width) * height, 0);

        RHI::TextureDesc texDesc{};
        texDesc.width = width;
        texDesc.height = height;
        // R8：字形是单通道覆盖率。片元着色器把它当 alpha，rgb 取顶点色，
        // 于是同一份图集可以画任意颜色的文字。
        texDesc.format = RHI::Format::R8Unorm;
        texDesc.usage = RHI::TextureUsage::Sampled;
        texDesc.debugName = "RxFontAtlas";

        const RHI::TextureHandle rhiTexture = device->createTexture(texDesc);
        if (!rhiTexture.valid())
        {
            log.error("[rt] rxFontCreate: 图集纹理创建失败");
            delete font;
            return RxResult::ErrorUnknown;
        }
        // 登记进公共纹理表，这样图集能像普通纹理一样填进 DrawCommand::texture，
        // 并共享 bindGroupForTexture 的绑定组缓存。
        font->texture = static_cast<TextureHandle>(textures.insert(rhiTexture));

        // 先把全零图集推上去：调用方可能在任何字形入图集之前就提交了绘制，
        // 未初始化的纹理内容在各后端上是未定义的（会出现随机噪点块）。
        const RHI::Rect2D full{ 0, 0, width, height };
        device->writeTexture(rhiTexture, 0, full, font->pixels.data(), font->pixels.size());

        *outFont = static_cast<FontHandle>(fonts.insert(font));
        log.info("[rt] 字体就绪：pixelHeight=%.1f 图集 %ux%u（R8 %s，%.2f MB）",
                 static_cast<double>(desc.pixelHeight), width, height,
                 desc.sdfPadding != 0 ? "距离场" : "覆盖率",
                 static_cast<double>(font->pixels.size()) / (1024.0 * 1024.0));
        return RxResult::Ok;
    }

    void Runtime::destroyFont(FontHandle handle)
    {
        Font** found = fonts.find(static_cast<uint64_t>(handle));
        if (!found || !*found)
        {
            log.warn("[rt] rxFontDestroy: 句柄无效或已销毁");
            return;
        }
        Font* font = *found;
        // 走 destroyTexture 而不是直接 device->destroyTexture：前者还会把
        // 该纹理的绑定组一并销毁，否则句柄值被复用时会拿到指向旧纹理的绑定组。
        if (rxValid(font->texture))
        {
            destroyTexture(font->texture);
        }
        delete font;
        fonts.erase(static_cast<uint64_t>(handle));
    }

    Font* Runtime::resolveFont(FontHandle handle)
    {
        Font** found = fonts.find(static_cast<uint64_t>(handle));
        return found ? *found : nullptr;
    }

    void Runtime::destroyAllFonts()
    {
        // 只释放 CPU 侧对象：图集纹理与其绑定组由 destroy() 里统一的
        // textures / textureBindGroups 清理循环负责，这里再销毁一次就是双重释放。
        for (Font* font : fonts)
        {
            delete font;
        }
        fonts.clear();
    }

}  // namespace Render::RT::detail

// ==================== 图集打包与查询 ====================
//
// 这几个函数不是 Runtime 的成员：它们只操作 Font，放进 Runtime 会让
// Runtime 的接口膨胀出一批与其他资源无关的方法。C API 层直接调它们。

namespace Render::RT::detail
{
    namespace
    {
        /// 把字形位图放进图集，返回左上角坐标；放不下返回 false
        bool packGlyph(Font& font, uint32_t glyphWidth, uint32_t glyphHeight, uint32_t& outX,
                       uint32_t& outY)
        {
            const uint32_t needW = glyphWidth + kGlyphPadding;
            const uint32_t needH = glyphHeight + kGlyphPadding;

            if (font.cursorX + needW > font.atlasWidth)
            {
                // 换行
                font.cursorX = 0;
                font.cursorY += font.rowHeight;
                font.rowHeight = 0;
            }
            if (font.cursorY + needH > font.atlasHeight)
            {
                return false;
            }

            outX = font.cursorX;
            outY = font.cursorY;
            font.cursorX += needW;
            font.rowHeight = std::max(font.rowHeight, needH);
            return true;
        }
    }  // namespace

    RxResult fontGlyph(Runtime& runtime, FontHandle handle, uint32_t codepoint, GlyphInfo* outGlyph)
    {
        if (!outGlyph)
        {
            return RxResult::ErrorInvalidArgument;
        }
        Font* fontPtr = runtime.resolveFont(handle);
        if (!fontPtr)
        {
            return RxResult::ErrorInvalidHandle;
        }
        Font& font = *fontPtr;
        auto cached = font.glyphs.find(codepoint);
        if (cached != font.glyphs.end())
        {
            *outGlyph = cached->second;
            return RxResult::Ok;
        }

        GlyphInfo info{};

        const int glyphIndex = stbtt_FindGlyphIndex(&font.info, static_cast<int>(codepoint));
        if (glyphIndex == 0)
        {
            // 字体里没有该码点。这不是错误：CAD 图纸里出现字体不覆盖的字符很常见，
            // 整行排版不该因此中断。缓存全零结果，避免每帧重复查同一个缺字。
            font.glyphs.emplace(codepoint, info);
            *outGlyph = info;
            return RxResult::Ok;
        }

        int advanceUnits = 0;
        int leftSideBearing = 0;
        stbtt_GetGlyphHMetrics(&font.info, glyphIndex, &advanceUnits, &leftSideBearing);
        info.advance = static_cast<float>(advanceUnits) * font.scale;

        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetGlyphBitmapBox(&font.info, glyphIndex, font.scale, font.scale, &x0, &y0, &x1, &y1);
        const int bitmapW = x1 - x0;
        const int bitmapH = y1 - y0;

        if (bitmapW <= 0 || bitmapH <= 0)
        {
            // 空白字形（空格、不可见控制符）：有步进但没有像素。
            // advance 已填好，UV 与尺寸留 0，调用方据此跳过四边形。
            font.glyphs.emplace(codepoint, info);
            *outGlyph = info;
            return RxResult::Ok;
        }

        // 光栅化产物的尺寸与相对笔位置的偏移。两种模式的差别集中在这一段：
        // 覆盖率模式直接用 GetGlyphBitmapBox 的结果；SDF 模式必须先生成，
        // 因为距离场向四周各外扩 padding 像素，尺寸与偏移都和墨迹范围不同。
        int glyphW = 0;
        int glyphH = 0;
        int offX = 0;
        int offY = 0;
        /// 仅 SDF 模式非空，用完必须 stbtt_FreeSDF（stb 内部 malloc）
        unsigned char* sdfPixels = nullptr;

        if (font.sdfPadding != 0)
        {
            const int padding = static_cast<int>(font.sdfPadding);
            // onedge_value=128：恰在轮廓上的像素值，于是片元里减 0.5 即得符号距离。
            // pixel_dist_scale=128/padding：偏离 1 像素变化多少级灰度。
            // 两者共同决定可表达的距离范围恰为 ±padding 像素 —— 超出即饱和，
            // 表现为放大到极限时边缘出现台阶，所以 padding 不能太小。
            sdfPixels = stbtt_GetGlyphSDF(&font.info, font.scale, glyphIndex, padding, 128,
                                          128.0f / static_cast<float>(padding), &glyphW, &glyphH,
                                          &offX, &offY);
            if (!sdfPixels || glyphW <= 0 || glyphH <= 0)
            {
                // 距离场生成失败：按缺字处理并缓存，不中断整行排版，也不每帧重试。
                if (sdfPixels)
                {
                    stbtt_FreeSDF(sdfPixels, nullptr);
                }
                font.glyphs.emplace(codepoint, info);
                *outGlyph = info;
                return RxResult::Ok;
            }
        }
        else
        {
            glyphW = bitmapW;
            glyphH = bitmapH;
            offX = x0;
            offY = y0;
        }

        uint32_t atlasX = 0;
        uint32_t atlasY = 0;
        if (!packGlyph(font, static_cast<uint32_t>(glyphW), static_cast<uint32_t>(glyphH), atlasX,
                       atlasY))
        {
            if (sdfPixels)
            {
                stbtt_FreeSDF(sdfPixels, nullptr);
            }
            if (!font.warnedFull)
            {
                font.warnedFull = true;
                runtime.log.error("[rt] 字形图集 %ux%u 已满，后续字形无法入集；"
                                  "请用更大的 FontDesc::atlasWidth/atlasHeight 重建字体",
                                  font.atlasWidth, font.atlasHeight);
            }
            // 不缓存：换更大的图集重建后仍应能光栅化。
            *outGlyph = GlyphInfo{};
            return RxResult::ErrorOutOfMemory;
        }

        if (sdfPixels)
        {
            // 距离场由 stb 生成在紧凑缓冲里，逐行搬进图集（图集步长是整张宽度）
            for (int row = 0; row < glyphH; ++row)
            {
                std::memcpy(&font.pixels[(static_cast<size_t>(atlasY) + row) * font.atlasWidth + atlasX],
                            sdfPixels + static_cast<size_t>(row) * glyphW,
                            static_cast<size_t>(glyphW));
            }
            stbtt_FreeSDF(sdfPixels, nullptr);
        }
        else
        {
            // 覆盖率位图可直接光栅化进 CPU 影子，步长是整张图集的宽度
            stbtt_MakeGlyphBitmap(&font.info,
                                  &font.pixels[static_cast<size_t>(atlasY) * font.atlasWidth + atlasX],
                                  glyphW, glyphH, static_cast<int>(font.atlasWidth), font.scale,
                                  font.scale, glyphIndex);
        }
        font.markDirtyRows(atlasY, atlasY + static_cast<uint32_t>(glyphH));

        const float invW = 1.0f / static_cast<float>(font.atlasWidth);
        const float invH = 1.0f / static_cast<float>(font.atlasHeight);
        info.u0 = static_cast<float>(atlasX) * invW;
        info.v0 = static_cast<float>(atlasY) * invH;
        info.u1 = static_cast<float>(atlasX + static_cast<uint32_t>(glyphW)) * invW;
        info.v1 = static_cast<float>(atlasY + static_cast<uint32_t>(glyphH)) * invH;
        // offX/offY 是相对**基线上笔位置**的偏移，y 向下为正，
        // 因此 offY 通常是负值（字形主体在基线之上）。与 GlyphInfo 的约定一致。
        // SDF 模式下这两个值已含 padding 外扩，四边形因此比墨迹大一圈 ——
        // 这是必须的：距离场在轮廓外侧仍有有效数据，裁掉就没有抗锯齿过渡了。
        info.bearingX = static_cast<float>(offX);
        info.bearingY = static_cast<float>(offY);
        info.width = static_cast<float>(glyphW);
        info.height = static_cast<float>(glyphH);

        font.glyphs.emplace(codepoint, info);
        *outGlyph = info;
        return RxResult::Ok;
    }

    RxResult fontMetrics(Runtime& runtime, FontHandle handle, FontMetrics* outMetrics)
    {
        if (!outMetrics)
        {
            return RxResult::ErrorInvalidArgument;
        }
        Font* font = runtime.resolveFont(handle);
        if (!font)
        {
            return RxResult::ErrorInvalidHandle;
        }
        *outMetrics = font->metrics;
        return RxResult::Ok;
    }

    TextureHandle fontAtlas(Runtime& runtime, FontHandle handle)
    {
        Font* font = runtime.resolveFont(handle);
        return font ? font->texture : TextureHandle::Invalid;
    }

    RxResult fontFlushAtlas(Runtime& runtime, FontHandle handle)
    {
        Font* fontPtr = runtime.resolveFont(handle);
        if (!fontPtr)
        {
            return RxResult::ErrorInvalidHandle;
        }
        Font& font = *fontPtr;
        if (!font.hasDirty())
        {
            return RxResult::Ok;
        }
        const RHI::TextureHandle* rhiTexture = runtime.textures.find(static_cast<uint64_t>(font.texture));
        if (!rhiTexture)
        {
            return RxResult::ErrorInvalidHandle;
        }

        const uint32_t rows = font.dirtyY1 - font.dirtyY0;
        const RHI::Rect2D region{ 0, static_cast<int32_t>(font.dirtyY0), font.atlasWidth, rows };
        const size_t offset = static_cast<size_t>(font.dirtyY0) * font.atlasWidth;
        const size_t bytes = static_cast<size_t>(rows) * font.atlasWidth;

        const RHI::RhiResult result =
            runtime.device->writeTexture(*rhiTexture, 0, region, font.pixels.data() + offset, bytes);
        // 无论成功与否都清脏区：失败是后端问题，反复重传同一批字节只会把
        // 每帧都变成一次失败的全量上传。
        font.dirtyY0 = 0;
        font.dirtyY1 = 0;
        return result == RHI::RhiResult::Ok ? RxResult::Ok : RxResult::ErrorUnknown;
    }

}  // namespace Render::RT::detail
