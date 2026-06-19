#include "Render2D/TextRenderer.h"
#include <stb_truetype.h>

#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QFile>
#include <QDebug>
#include <fstream>
#include <cmath>
#include <cstring>

namespace Rd
{

// ==================== 单例 ====================

TextRenderer& TextRenderer::instance()
{
    static TextRenderer s_instance;
    return s_instance;
}

// ==================== 构造/析构 ====================

TextRenderer::TextRenderer() = default;

TextRenderer::~TextRenderer()
{
    cleanup();
}

// ==================== 初始化/清理 ====================

bool TextRenderer::initialize(int atlasWidth, int atlasHeight)
{
    if (m_atlasTexture != 0)
        return true; // 已初始化

    m_atlasWidth = atlasWidth;
    m_atlasHeight = atlasHeight;

    // 创建纹理图集
    if (!initAtlas(atlasWidth, atlasHeight))
        return false;

    // 创建 OpenGL 资源
    auto* f = QOpenGLContext::currentContext()->extraFunctions();
    f->glGenVertexArrays(1, &m_vao);
    f->glGenBuffers(1, &m_vbo);
    f->glGenBuffers(1, &m_ebo);

    // 创建着色器程序
    m_textProgram = new QOpenGLShaderProgram();

    const char* vertexShader = R"(
        #version 330 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aTexCoord;
        layout(location = 2) in vec4 aColor;

        uniform mat3 uProjMatrix;
        uniform int uScreenSpace;

        out vec2 vTexCoord;
        out vec4 vColor;

        void main()
        {
            vec3 pos = uProjMatrix * vec3(aPos, 1.0);
            gl_Position = vec4(pos.xy, 0.0, pos.z);
            vTexCoord = aTexCoord;
            vColor = aColor;
        }
    )";

    const char* fragmentShader = R"(
        #version 330 core
        in vec2 vTexCoord;
        in vec4 vColor;

        uniform sampler2D uTexture;

        out vec4 FragColor;

        void main()
        {
            float alpha = texture(uTexture, vTexCoord).r;
            FragColor = vec4(vColor.rgb, vColor.a * alpha);
        }
    )";

    if (!m_textProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader))
    {
        qWarning() << "TextRenderer: Vertex shader compilation failed:" << m_textProgram->log();
        return false;
    }
    if (!m_textProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader))
    {
        qWarning() << "TextRenderer: Fragment shader compilation failed:" << m_textProgram->log();
        return false;
    }
    if (!m_textProgram->link())
    {
        qWarning() << "TextRenderer: Shader program link failed:" << m_textProgram->log();
        return false;
    }

    // 预分配缓冲区
    m_vertices.reserve(4096);
    m_indices.reserve(6144);

    return true;
}

void TextRenderer::cleanup()
{
    auto* f = QOpenGLContext::currentContext();
    if (!f) return;

    auto* gl = f->extraFunctions();

    if (m_vao)  { gl->glDeleteVertexArrays(1, &m_vao);  m_vao = 0; }
    if (m_vbo)  { gl->glDeleteBuffers(1, &m_vbo);       m_vbo = 0; }
    if (m_ebo)  { gl->glDeleteBuffers(1, &m_ebo);       m_ebo = 0; }
    if (m_atlasTexture) { gl->glDeleteTextures(1, &m_atlasTexture); m_atlasTexture = 0; }

    delete m_textProgram;
    m_textProgram = nullptr;

    freeAtlas();
    m_fonts.clear();
    m_glyphCache.clear();
}

// ==================== 字体管理 ====================

bool TextRenderer::loadFont(const std::string& fontPath, float fontSize)
{
    std::ifstream file(fontPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        qWarning() << "TextRenderer: Cannot open font file:" << fontPath.c_str();
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<unsigned char> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
    {
        qWarning() << "TextRenderer: Failed to read font file:" << fontPath.c_str();
        return false;
    }

    return loadFontFromMemory(buffer.data(), buffer.size(), fontSize);
}

bool TextRenderer::loadFontFromMemory(const unsigned char* data, size_t size, float fontSize)
{
    int fontSizeKey = static_cast<int>(fontSize * 100.0f);

    auto fontData = std::make_unique<FontData>();
    fontData->ttfData.assign(data, data + size);
    fontData->fontSize = fontSize;

    int offset = stbtt_GetFontOffsetForIndex(fontData->ttfData.data(), 0);
    if (offset < 0)
    {
        qWarning() << "TextRenderer: Invalid font data";
        return false;
    }

    if (!stbtt_InitFont(&fontData->info, fontData->ttfData.data(), offset))
    {
        qWarning() << "TextRenderer: Failed to init font";
        return false;
    }

    fontData->scale = stbtt_ScaleForPixelHeight(&fontData->info, fontSize);

    stbtt_GetFontVMetrics(&fontData->info, &fontData->ascent, &fontData->descent, &fontData->lineGap);

    m_fonts[fontSizeKey] = std::move(fontData);
    return true;
}

void TextRenderer::setDefaultFontSize(float size)
{
    m_defaultFontSize = static_cast<int>(size);
}

TextRenderer::FontData* TextRenderer::getCurrentFont(float fontSize)
{
    int key = static_cast<int>(fontSize * 100.0f);
    auto it = m_fonts.find(key);
    if (it != m_fonts.end())
        return it->second.get();

    // 回退到默认字号
    int defaultKey = static_cast<int>(m_defaultFontSize * 100.0f);
    it = m_fonts.find(defaultKey);
    if (it != m_fonts.end())
        return it->second.get();

    // 返回任意可用字体
    if (!m_fonts.empty())
        return m_fonts.begin()->second.get();

    return nullptr;
}

// ==================== 纹理图集 ====================

bool TextRenderer::initAtlas(int width, int height)
{
    m_atlasPixels = std::make_unique<unsigned char[]>(static_cast<size_t>(width) * height);
    std::memset(m_atlasPixels.get(), 0, static_cast<size_t>(width) * height);

    m_atlasRoot = new AtlasNode{0, 0, width, height};

    auto* f = QOpenGLContext::currentContext()->extraFunctions();
    f->glGenTextures(1, &m_atlasTexture);
    f->glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return true;
}

void TextRenderer::freeAtlas()
{
    delete m_atlasRoot;
    m_atlasRoot = nullptr;
    m_atlasPixels.reset();
}

TextRenderer::AtlasNode* TextRenderer::insertNode(AtlasNode* node, int w, int h)
{
    if (!node) return nullptr;

    // 如果节点已被占用，尝试左右子树
    if (node->left || node->right)
    {
        AtlasNode* result = insertNode(node->left, w, h);
        if (result) return result;
        return insertNode(node->right, w, h);
    }

    // 节点太小
    if (w > node->w || h > node->h)
        return nullptr;

    // 完美匹配
    if (w == node->w && h == node->h)
        return node;

    // 分割节点
    node->left = new AtlasNode{node->x, node->y, 0, 0};
    node->right = new AtlasNode{node->x, node->y, 0, 0};

    int dw = node->w - w;
    int dh = node->h - h;

    if (dw > dh)
    {
        // 水平分割
        node->left->x = node->x;
        node->left->y = node->y;
        node->left->w = w;
        node->left->h = node->h;

        node->right->x = node->x + w;
        node->right->y = node->y;
        node->right->w = dw;
        node->right->h = node->h;
    }
    else
    {
        // 垂直分割
        node->left->x = node->x;
        node->left->y = node->y;
        node->left->w = node->w;
        node->left->h = h;

        node->right->x = node->x;
        node->right->y = node->y + h;
        node->right->w = node->w;
        node->right->h = dh;
    }

    return insertNode(node->left, w, h);
}

bool TextRenderer::packGlyph(int glyphW, int glyphH, int& outX, int& outY)
{
    AtlasNode* node = insertNode(m_atlasRoot, glyphW, glyphH);
    if (!node)
        return false;

    outX = node->x;
    outY = node->y;
    return true;
}

// ==================== 字形光栅化 ====================

bool TextRenderer::rasterizeGlyph(int codepoint, float fontSize, GlyphInfo& outGlyph)
{
    FontData* font = getCurrentFont(fontSize);
    if (!font)
        return false;

    float scale = stbtt_ScaleForPixelHeight(&font->info, fontSize);

    int glyphIndex = stbtt_FindGlyphIndex(&font->info, codepoint);
    if (glyphIndex == 0 && codepoint != 0)
        return false;

    int advanceWidth = 0, leftSideBearing = 0;
    stbtt_GetGlyphHMetrics(&font->info, glyphIndex, &advanceWidth, &leftSideBearing);

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&font->info, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);

    int glyphW = x1 - x0;
    int glyphH = y1 - y0;

    // 空格等无位图字符
    if (glyphW <= 0 || glyphH <= 0)
    {
        outGlyph.codepoint = codepoint;
        outGlyph.x0 = 0; outGlyph.y0 = 0;
        outGlyph.x1 = 0; outGlyph.y1 = 0;
        outGlyph.xoff = static_cast<float>(leftSideBearing) * scale;
        outGlyph.yoff = 0;
        outGlyph.xadvance = static_cast<float>(advanceWidth) * scale;
        return true;
    }

    // 在纹理图集中分配空间
    int atlasX = 0, atlasY = 0;
    if (!packGlyph(glyphW + 1, glyphH + 1, atlasX, atlasY))
    {
        qWarning() << "TextRenderer: Atlas full, cannot pack glyph";
        return false;
    }

    // 光栅化字形
    std::vector<unsigned char> bitmap(static_cast<size_t>(glyphW) * glyphH);
    stbtt_MakeGlyphBitmap(&font->info, bitmap.data(), glyphW, glyphH, glyphW,
        scale, scale, glyphIndex);

    // 复制到图集
    for (int row = 0; row < glyphH; ++row)
    {
        std::memcpy(
            &m_atlasPixels[(atlasY + row) * m_atlasWidth + atlasX],
            &bitmap[row * glyphW],
            glyphW
        );
    }

    // 更新纹理
    auto* f = QOpenGLContext::currentContext()->extraFunctions();
    f->glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
    f->glTexSubImage2D(GL_TEXTURE_2D, 0, atlasX, atlasY, glyphW, glyphH,
        GL_RED, GL_UNSIGNED_BYTE, bitmap.data());

    outGlyph.codepoint = codepoint;
    outGlyph.x0 = static_cast<float>(atlasX) / m_atlasWidth;
    outGlyph.y0 = static_cast<float>(atlasY) / m_atlasHeight;
    outGlyph.x1 = static_cast<float>(atlasX + glyphW) / m_atlasWidth;
    outGlyph.y1 = static_cast<float>(atlasY + glyphH) / m_atlasHeight;
    outGlyph.xoff = static_cast<float>(x0);
    outGlyph.yoff = static_cast<float>(y0);
    outGlyph.xadvance = static_cast<float>(advanceWidth) * scale;

    return true;
}

const TextRenderer::GlyphInfo* TextRenderer::getGlyph(int codepoint, float fontSize)
{
    uint64_t key = (static_cast<uint64_t>(static_cast<int>(fontSize * 100.0f)) << 32) |
                   static_cast<uint64_t>(codepoint);

    auto it = m_glyphCache.find(key);
    if (it != m_glyphCache.end())
        return &it->second;

    GlyphInfo glyph;
    if (!rasterizeGlyph(codepoint, fontSize, glyph))
        return nullptr;

    m_glyphCache[key] = glyph;
    return &m_glyphCache[key];
}

// ==================== 预加载 ====================

void TextRenderer::preloadGlyphs(const std::string& text, float fontSize)
{
    for (unsigned char c : text)
    {
        getGlyph(static_cast<int>(c), fontSize);
    }
}

// ==================== 测量 ====================

Render::Vec2f TextRenderer::measureText(const std::string& text, float fontSize) const
{
    float width = 0.0f;
    float maxHeight = 0.0f;

    FontData* font = nullptr;
    int key = static_cast<int>(fontSize * 100.0f);
    auto it = m_fonts.find(key);
    if (it != m_fonts.end())
        font = it->second.get();

    for (unsigned char c : text)
    {
        if (c == '\n')
        {
            // 换行处理（简化：仅记录最大宽度）
            continue;
        }

        if (font)
        {
            int glyphIndex = stbtt_FindGlyphIndex(&font->info, c);
            int advanceWidth = 0;
            stbtt_GetGlyphHMetrics(&font->info, glyphIndex, &advanceWidth, nullptr);
            float scale = stbtt_ScaleForPixelHeight(&font->info, fontSize);
            width += static_cast<float>(advanceWidth) * scale;

            int ascent, descent, lineGap;
            stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &lineGap);
            float lineHeight = static_cast<float>(ascent - descent + lineGap) * scale;
            if (lineHeight > maxHeight)
                maxHeight = lineHeight;
        }
    }

    return Render::Vec2f(width, maxHeight);
}

// ==================== 帧绘制 ====================

void TextRenderer::beginFrame(int viewportWidth, int viewportHeight, const Ut::Mat3f& viewMatrix)
{
    m_vpWidth = viewportWidth;
    m_vpHeight = viewportHeight;
    m_viewMatrix = viewMatrix;
    m_vertices.clear();
    m_indices.clear();
}

void TextRenderer::drawText(const TextDrawInfo& info)
{
    if (info.text.empty() || !m_atlasTexture)
        return;

    float fontSize = info.fontSize > 0 ? info.fontSize : static_cast<float>(m_defaultFontSize);
    float penX = 0.0f;
    float penY = 0.0f;

    // 计算文本总尺寸用于锚点
    Render::Vec2f textSize = measureText(info.text, fontSize);
    float offsetX = -textSize.x() * info.anchor.x();
    float offsetY = -textSize.y() * info.anchor.y();
    penX += offsetX;
    penY += offsetY;

    Render::Color color = info.color;

    for (unsigned char c : info.text)
    {
        if (c == '\n')
        {
            penX = offsetX;
            penY -= fontSize * 1.2f;
            continue;
        }

        const GlyphInfo* glyph = getGlyph(static_cast<int>(c), fontSize);
        if (!glyph || glyph->xadvance <= 0.0f)
        {
            // 未知字符，跳过
            penX += fontSize * 0.5f;
            continue;
        }

        float x0 = info.position.x() + penX + glyph->xoff;
        float y0 = info.position.y() + penY + glyph->yoff;
        float x1 = x0 + (glyph->x1 - glyph->x0) * m_atlasWidth;
        float y1 = y0 + (glyph->y1 - glyph->y0) * m_atlasHeight;

        // 处理旋转
        float cosR = 1.0f, sinR = 0.0f;
        if (info.rotation != 0.0f)
        {
            cosR = std::cos(info.rotation);
            sinR = std::sin(info.rotation);
        }

        auto rotatePoint = [&](float px, float py) -> std::pair<float, float> {
            float dx = px - info.position.x();
            float dy = py - info.position.y();
            return {
                info.position.x() + dx * cosR - dy * sinR,
                info.position.y() + dx * sinR + dy * cosR
            };
        };

        auto [rx0, ry0] = rotatePoint(x0, y0);
        auto [rx1, ry1] = rotatePoint(x1, y0);
        auto [rx2, ry2] = rotatePoint(x1, y1);
        auto [rx3, ry3] = rotatePoint(x0, y1);

        GLuint baseIndex = static_cast<GLuint>(m_vertices.size());

        m_vertices.push_back({rx0, ry0, glyph->x0, glyph->y0, color.r(), color.g(), color.b(), color.a()});
        m_vertices.push_back({rx1, ry1, glyph->x1, glyph->y0, color.r(), color.g(), color.b(), color.a()});
        m_vertices.push_back({rx2, ry2, glyph->x1, glyph->y1, color.r(), color.g(), color.b(), color.a()});
        m_vertices.push_back({rx3, ry3, glyph->x0, glyph->y1, color.r(), color.g(), color.b(), color.a()});

        m_indices.push_back(baseIndex);
        m_indices.push_back(baseIndex + 1);
        m_indices.push_back(baseIndex + 2);
        m_indices.push_back(baseIndex);
        m_indices.push_back(baseIndex + 2);
        m_indices.push_back(baseIndex + 3);

        penX += glyph->xadvance;
    }
}

void TextRenderer::endFrame()
{
    flushBatch();
}

void TextRenderer::flushBatch()
{
    if (m_vertices.empty() || !m_textProgram || !m_atlasTexture)
        return;

    auto* f = QOpenGLContext::currentContext()->extraFunctions();

    // 绑定 VAO
    f->glBindVertexArray(m_vao);

    // 上传顶点数据
    f->glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    f->glBufferData(GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(m_vertices.size() * sizeof(Vertex)),
        m_vertices.data(), GL_DYNAMIC_DRAW);

    // 上传索引数据
    f->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    f->glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(m_indices.size() * sizeof(GLuint)),
        m_indices.data(), GL_DYNAMIC_DRAW);

    // 设置顶点属性
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        reinterpret_cast<void*>(offsetof(Vertex, x)));
    f->glEnableVertexAttribArray(0);

    f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        reinterpret_cast<void*>(offsetof(Vertex, u)));
    f->glEnableVertexAttribArray(1);

    f->glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        reinterpret_cast<void*>(offsetof(Vertex, r)));
    f->glEnableVertexAttribArray(2);

    // 使用着色器
    m_textProgram->bind();

    // 设置投影矩阵
    // 使用正交投影：left=0, right=viewportWidth, bottom=viewportHeight, top=0 (屏幕坐标)
    Ut::Mat3f projMatrix = Ut::Mat3f::ortho2D(0.0f, static_cast<float>(m_vpWidth),
        static_cast<float>(m_vpHeight), 0.0f);

    // 构造 9 个浮点的一维数组（按行主序）以便 QMatrix3x3 接受
    float matData[9] = {
        projMatrix.data[0], projMatrix.data[3], projMatrix.data[6],
        projMatrix.data[1], projMatrix.data[4], projMatrix.data[7],
        projMatrix.data[2], projMatrix.data[5], projMatrix.data[8]
    };
    m_textProgram->setUniformValue("uProjMatrix", QMatrix3x3(matData));

    m_textProgram->setUniformValue("uScreenSpace", 1);

    // 绑定纹理
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, m_atlasTexture);
    m_textProgram->setUniformValue("uTexture", 0);

    // 启用混合
    f->glEnable(GL_BLEND);
    f->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 绘制
    f->glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_indices.size()),
        GL_UNSIGNED_INT, nullptr);

    // 清理
    f->glDisable(GL_BLEND);
    f->glBindVertexArray(0);
    m_textProgram->release();
}

} // namespace Rd
