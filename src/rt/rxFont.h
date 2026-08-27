/**
 * @file rxFont.h
 * @brief 字形图集：字体光栅化 + R8 图集打包 + 度量缓存
 *
 * 这里**只做字形**，不做文本。DLL 里没有「一段文字」这个概念：
 * UTF-8 解码、字距、换行、对齐、旋转、世界/屏幕坐标换算全在调用方，
 * 调用方拿 GlyphInfo 自己拼 P2T2C4 四边形。
 *
 * 这与被删除的 core/textAtlas + core/screenTextRenderer 方向相反：那两个类
 * 接收字符串、内部排版、并自己 bindPipeline + draw。代价是文本永远是独立的
 * 一批 draw call，无法与其他图元一起参与 sortKey 排序与批次合并，
 * 且「对齐规则」这种纯业务约定被编进了渲染 DLL。
 *
 * 与旧实现的其余差异（都是旧实现的缺陷，不是取舍）：
 * - 图集是 R8 而非 RGBA8。旧实现把 8 位覆盖率复制到四个通道，
 *   2048x2048 的图集占 16MB，其中 12MB 是同一份数据的副本。
 * - 上传按脏行增量，而非每次 renderText 全量重传整张图集。
 * - 字形缓存是哈希表，而非对 vector 做线性扫描。
 * - 字体字节在 DLL 内留副本：stbtt_fontinfo 持有原始数据指针，
 *   不留副本则调用方一释放就是悬垂指针（旧 textAtlas 正是如此）。
 *
 * ## 两种图集内容：覆盖率 与 SDF
 *
 * `FontDesc::sdfPadding == 0` 时图集存**覆盖率**（`stbtt_MakeGlyphBitmap`），
 * 像素值就是该点被字形覆盖的比例。屏幕空间文字用这个：显示尺寸等于光栅化
 * 尺寸，一一对应，最锐利。
 *
 * `sdfPadding > 0` 时图集存**有符号距离场**（`stbtt_GetGlyphSDF`），
 * 128 表示恰在轮廓上。世界空间文字用这个：显示尺寸随缩放连续变化，
 * 距离场可任意插值，一个句柄覆盖所有尺寸，不必按缩放档位重建图集。
 *
 * 曾经存在的 `text_sdf.frag` 与此**不是一回事**，它对覆盖率位图做 smoothstep
 * 并当作距离场解释 —— 那等于硬阈值化，反而削掉了抗锯齿边缘。距离场必须在
 * 光栅化阶段真的生成，不能在采样阶段假装。这也是那条路径当初被删掉的原因。
 */
#pragma once

#include "rt/rxInternal.h"

#include <stb_truetype.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Render::RT::detail
{

    struct Font
    {
        Runtime* runtime = nullptr;

        /// 字体字节副本（见文件头：stbtt_fontinfo 持有指针）
        std::vector<uint8_t> data;
        stbtt_fontinfo info{};
        /// 字体单位 → 像素的比例，stbtt_ScaleForPixelHeight 的结果
        float scale = 1.0f;
        FontMetrics metrics{};

        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        /// R8 内容，行优先、行间无填充（步长恒为 atlasWidth）。
        /// 覆盖率或距离场，取决于 sdfPadding，见文件头。
        std::vector<uint8_t> pixels;
        TextureHandle texture = TextureHandle::Invalid;

        /// 0 = 覆盖率位图；>0 = SDF 模式的边缘留白（像素）
        uint32_t sdfPadding = 0;

        /// 行式打包游标：从左到右填一行，填满换行
        uint32_t cursorX = 0;
        uint32_t cursorY = 0;
        /// 当前行已用的最大高度，换行时据此下移
        uint32_t rowHeight = 0;

        std::unordered_map<uint32_t, GlyphInfo> glyphs;

        /**
         * 待上传的脏行区间 [dirtyY0, dirtyY1)。
         *
         * 只按**行**记录而不是任意矩形：整行的像素在 CPU 影子里是连续的，
         * 可以直接把 &pixels[dirtyY0 * atlasWidth] 交给 writeTexture；
         * 任意矩形则要先重排到紧凑暂存区，为省几 KB 带宽多一次拷贝不值得。
         */
        uint32_t dirtyY0 = 0;
        uint32_t dirtyY1 = 0;

        /// 图集填满后只告警一次，否则每个字形一条日志会把日志刷爆
        bool warnedFull = false;

        bool hasDirty() const
        {
            return dirtyY1 > dirtyY0;
        }

        void markDirtyRows(uint32_t y0, uint32_t y1)
        {
            if (!hasDirty())
            {
                dirtyY0 = y0;
                dirtyY1 = y1;
                return;
            }
            dirtyY0 = y0 < dirtyY0 ? y0 : dirtyY0;
            dirtyY1 = y1 > dirtyY1 ? y1 : dirtyY1;
        }
    };

}  // namespace Render::RT::detail
