/**
 * @file text_atlas.h
 * @brief 文本贴图管理器类定义
 * 
 * TextAtlas 负责文本渲染，包括：
 * - 字体加载和栅格化
 * - Glyph 缓存和图集管理
 * - 文本四边形构建
 * - 带纹理的文本渲染
 * 
 * 使用 stb_truetype 进行字体栅格化，将字符缓存到纹理图集中。
 */
#pragma once

#include "render/render_types.h"
#include "../rhi/rhi_device.h"
#include <vector>
#include <cstdint>
#include <string>

namespace render::core {

/**
 * @brief 文本贴图管理器类
 * 
 * 管理字体资源和字符图集，负责文本渲染。
 */
class TextAtlas
{
public:
    /**
     * @brief 初始化文本贴图管理器
     * 
     * @param device RHI设备指针
     * @return 初始化是否成功
     */
    bool initialize(rhi::IDevice* device);

    /**
     * @brief 关闭并释放所有资源
     */
    void shutdown();

    /**
     * @brief 加载字体数据
     * 
     * @param fontData 字体数据指针（TTF/OTF格式）
     * @param dataSize 字体数据大小
     * @param pixelHeight 字体像素高度（默认32像素）
     */
    void loadFont(const void* fontData, size_t dataSize, float pixelHeight = 32.0f);

    /**
     * @brief 渲染文本列表
     * 
     * @param texts 文本项列表
     * @param viewMatrix 3x3视图矩阵
     * @param device RHI设备指针
     */
    void renderText(const TextItemList* texts, const float viewMatrix[9], rhi::IDevice* device);

private:
    /**
     * @brief Glyph 信息结构
     * 
     * 存储单个字符的纹理坐标和几何信息。
     */
    struct GlyphInfo
    {
        float u0, v0, u1, v1; ///< 纹理坐标（归一化）
        float x0, y0, x1, y1; ///< 字符边界框（像素）
        float xAdvance;        ///< 字符宽度（像素）
        bool  valid;           ///< 是否有效
    };

    /**
     * @brief 缓存的 Glyph 结构
     * 
     * 存储在图集中的字符信息。
     */
    struct CachedGlyph
    {
        uint32_t  codepoint; ///< Unicode 码点
        float     fontSize;  ///< 字体大小
        GlyphInfo info;      ///< Glyph 信息
    };

    /**
     * @brief 文本顶点结构
     * 
     * 包含位置、纹理坐标和颜色信息。
     */
    struct TextVertex
    {
        float px, py, pz;    ///< 位置坐标
        float u, v;          ///< 纹理坐标
        float cr, cg, cb, ca; ///< 颜色（RGBA）
    };

    /// 静态断言：TextVertex 大小必须为36字节
    static_assert(sizeof(TextVertex) == 36, "TextVertex must be 36 bytes");

    /// RHI设备指针
    rhi::IDevice*       m_device        = nullptr;
    /// 图集纹理
    rhi::TextureHandle  m_atlasTexture  = rhi::NullHandle;
    /// 顶点缓冲区
    rhi::BufferHandle   m_vertexBuffer  = rhi::NullHandle;
    /// 文本渲染管线
    rhi::PipelineHandle m_textPipeline  = rhi::NullHandle;

    /// 图集像素数据
    std::vector<uint8_t>    m_atlasData;
    /// Glyph 缓存
    std::vector<CachedGlyph> m_glyphCache;

    /// 图集宽度（像素）
    uint32_t m_atlasWidth      = 2048;
    /// 图集高度（像素）
    uint32_t m_atlasHeight     = 2048;
    /// 图集当前X光标位置
    uint32_t m_atlasCursorX    = 0;
    /// 图集当前Y光标位置
    uint32_t m_atlasCursorY    = 0;
    /// 当前行高度
    uint32_t m_atlasLineHeight = 0;

    /// 字体像素高度
    float m_fontPixelHeight = 32.0f;

    /// stb_truetype 字体信息缓冲区
    char m_stbFontInfo[512] = {};
    /// 字体是否已加载
    bool m_fontLoaded       = false;

    /**
     * @brief 获取或栅格化 Glyph
     * 
     * 从缓存中查找，如果不存在则进行栅格化。
     * 
     * @param codepoint Unicode 码点
     * @param fontSize 字体大小
     * @return Glyph 信息
     */
    GlyphInfo getGlyph(uint32_t codepoint, float fontSize);

    /**
     * @brief 栅格化字符到图集
     * 
     * @param codepoint Unicode 码点
     * @param fontSize 字体大小
     * @param outInfo 输出 Glyph 信息
     * @return 是否成功
     */
    bool rasterizeGlyph(uint32_t codepoint, float fontSize, GlyphInfo* outInfo);

    /**
     * @brief 构建文本四边形
     * 
     * 将文本项转换为顶点数据。
     * 
     * @param item 文本项
     * @param outQuads 输出顶点数组
     * @param viewMatrix 3x3视图矩阵
     */
    void buildTextQuads(const TextItem& item, std::vector<TextVertex>& outQuads, const float viewMatrix[9]);
};

}
