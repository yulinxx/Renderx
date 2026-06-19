#pragma once

#include "RenderAPI.h"
#include "Render/RenderTypes.h"
#include <QOpenGLShaderProgram>
#include <QOpenGLExtraFunctions>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <stb_truetype.h>

namespace Rd
{
    /**
     * @brief 文本渲染器 - 使用 stb_truetype 光栅化字体到纹理图集
     *
     * 支持两种坐标系统：
     * 1. 屏幕坐标 (Screen Space) - 固定在屏幕上，不跟随视图变化
     * 2. 世界坐标但缩放不变 - 跟随视图平移，但文字大小保持像素不变
     */
    class RENDER_API TextRenderer
    {
    public:
        struct GlyphInfo
        {
            int codepoint = 0;
            float x0, y0, x1, y1;       // 纹理坐标 (0-1)
            float xoff, yoff;             // 绘制偏移
            float xadvance;               // 字符宽度
        };

        struct TextDrawInfo
        {
            std::string text;
            Render::Vec2f position;       // 世界坐标 或 屏幕像素坐标
            Render::Color color = Render::Color(1, 1, 1, 1);
            float fontSize = 12.0f;       // 像素大小
            bool screenSpace = false;     // true=屏幕坐标, false=世界坐标(但字号不随缩放变化)
            float zDepth = 1.0f;          // 渲染深度
            float rotation = 0.0f;        // 旋转角度(弧度)
            Render::Vec2f anchor = { 0, 0 };    // 锚点 (0,0)=左下, (0.5,0.5)=中心, (1,1)=右上
        };

        // 单例模式
        static TextRenderer& instance();

        TextRenderer();
        ~TextRenderer();

        // 初始化/清理 OpenGL 资源
        bool initialize(int atlasWidth = 2048, int atlasHeight = 2048);
        void cleanup();

        // 字体管理
        bool loadFont(const std::string& fontPath, float fontSize = 12.0f);
        bool loadFontFromMemory(const unsigned char* data, size_t size, float fontSize = 12.0f);
        void setDefaultFontSize(float size);

        // 文本绘制
        void beginFrame(int viewportWidth, int viewportHeight, const Ut::Mat3f& viewMatrix);
        void drawText(const TextDrawInfo& info);
        void endFrame();

        // 测量文本尺寸
        Render::Vec2f measureText(const std::string& text, float fontSize) const;

        // 预加载字符
        void preloadGlyphs(const std::string& text, float fontSize);

        // 获取纹理ID (用于绑定)
        GLuint getAtlasTexture() const
        {
            return m_atlasTexture;
        }

        // 设置默认颜色
        void setDefaultColor(const Render::Color& color)
        {
            m_defaultColor = color;
        }

    private:
        struct FontData
        {
            stbtt_fontinfo info;
            std::vector<unsigned char> ttfData;
            float fontSize = 12.0f;
            float scale = 1.0f;
            int ascent = 0, descent = 0, lineGap = 0;
        };

        struct AtlasNode
        {
            int x, y, w, h;
            AtlasNode* left = nullptr;
            AtlasNode* right = nullptr;
        };

        bool initAtlas(int width, int height);
        void freeAtlas();
        AtlasNode* insertNode(AtlasNode* node, int w, int h);
        bool packGlyph(int glyphW, int glyphH, int& outX, int& outY);

        bool rasterizeGlyph(int codepoint, float fontSize, GlyphInfo& outGlyph);
        const GlyphInfo* getGlyph(int codepoint, float fontSize);
        FontData* getCurrentFont(float fontSize);

        void flushBatch();

        // 字体缓存 (按字号分组)
        std::unordered_map<int, std::unique_ptr<FontData>> m_fonts; // key = fontSize * 100
        int m_defaultFontSize = 12;

        // 纹理图集
        GLuint m_atlasTexture = 0;
        int m_atlasWidth = 2048;
        int m_atlasHeight = 2048;
        std::unique_ptr<unsigned char[]> m_atlasPixels;
        AtlasNode* m_atlasRoot = nullptr;

        // 字形缓存: fontSize*100 + codepoint -> GlyphInfo
        std::unordered_map<uint64_t, GlyphInfo> m_glyphCache;

        // 批量绘制数据
        struct Vertex
        {
            float x, y;      // 位置
            float u, v;      // 纹理坐标
            float r, g, b, a; // 颜色
        };
        std::vector<Vertex> m_vertices;
        std::vector<GLuint> m_indices;

        // OpenGL 资源
        GLuint m_vao = 0, m_vbo = 0, m_ebo = 0;
        QOpenGLShaderProgram* m_textProgram = nullptr;

        // 当前帧参数
        int m_vpWidth = 0, m_vpHeight = 0;
        Ut::Mat3f m_viewMatrix;
        Render::Color m_defaultColor = Render::Color(1, 1, 1, 1);

        // 单例禁止拷贝
        TextRenderer(const TextRenderer&) = delete;
        TextRenderer& operator=(const TextRenderer&) = delete;
    };
}
