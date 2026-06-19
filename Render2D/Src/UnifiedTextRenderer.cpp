#include "Render2D/UnifiedTextRenderer.h"
#include "Render2D/TextRenderer.h"

#include <QPainter>
#include <QFontMetrics>
#include <QFont>
#include <cmath>

UnifiedTextRenderer::UnifiedTextRenderer() = default;

UnifiedTextRenderer::~UnifiedTextRenderer()
{
    cleanupGPU();
}

bool UnifiedTextRenderer::initializeGPU()
{
    if (m_gpuInitialized) return true;
    m_textRenderer = &Rd::TextRenderer::instance();
    m_gpuInitialized = m_textRenderer->initialize();
    return m_gpuInitialized;
}

void UnifiedTextRenderer::cleanupGPU()
{
    if (m_textRenderer)
    {
        m_textRenderer->cleanup();
    }
    m_gpuInitialized = false;
}

void UnifiedTextRenderer::beginFrame(int viewportWidth, int viewportHeight,
    const Ut::Mat3f& viewMatrix)
{
    if (m_useGPU && m_gpuInitialized && m_textRenderer)
    {
        m_textRenderer->beginFrame(viewportWidth, viewportHeight, viewMatrix);
    }
}

void UnifiedTextRenderer::endFrame()
{
    if (m_useGPU && m_gpuInitialized && m_textRenderer)
    {
        m_textRenderer->endFrame();
    }
}

void UnifiedTextRenderer::renderTextsGPU(const Render::UiTextItemList& texts)
{
    if (!m_useGPU || !m_gpuInitialized || !m_textRenderer)
        return;

    for (const auto& item : texts)
    {
        Rd::TextRenderer::TextDrawInfo info;
        info.text = item.text;
        info.fontSize = static_cast<float>(item.fontSize);
        info.color = item.color;

        // 屏幕空间文字
        if (item.coordMode == Render::UiTextCoordMode::PixelCoords)
        {
            info.screenSpace = true;
            info.position = Render::Vec2f(item.x, item.y);
        }
        else
        {
            info.screenSpace = false;
            info.position = Render::Vec2f(item.x, item.y);
        }

        info.rotation = item.rotationDeg * 3.1415926f / 180.0f;

        // 对齐方式转锚点
        float anchorX = 0.0f;
        float anchorY = 0.0f;
        switch (item.hAlign)
        {
            case Render::UiTextHAlign::Left:   anchorX = 0.0f; break;
            case Render::UiTextHAlign::Center: anchorX = 0.5f; break;
            case Render::UiTextHAlign::Right:  anchorX = 1.0f; break;
        }
        switch (item.vAlign)
        {
            case Render::UiTextVAlign::Top:    anchorY = 0.0f; break;
            case Render::UiTextVAlign::Middle: anchorY = 0.5f; break;
            case Render::UiTextVAlign::Bottom: anchorY = 1.0f; break;
        }
        info.anchor = Render::Vec2f(anchorX, anchorY);

        m_textRenderer->drawText(info);
    }
}

void UnifiedTextRenderer::renderTextsQPainter(QPainter& painter,
    const Render::UiTextItemList& texts,
    int viewportWidth, int viewportHeight,
    const Ut::Mat3f& viewMatrix)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    for (const auto& item : texts)
    {
        QFont font("Microsoft YaHei", item.fontSize);
        painter.setFont(font);
        painter.setPen(QColor::fromRgbF(item.color.r(), item.color.g(), item.color.b(),
            item.color.a()));

        QFontMetrics fm(font);

        float drawX = 0, drawY = 0;

        if (item.coordMode == Render::UiTextCoordMode::PixelCoords)
        {
            drawX = item.x;
            drawY = item.y;
        }
        else
        {
            // 世界坐标 → 屏幕坐标
            const float* m = &viewMatrix.data[0];
            float screenX = m[0] * item.x + m[1] * item.y + m[2];
            float screenY = m[3] * item.x + m[4] * item.y + m[5];
            // NDC → 屏幕像素
            drawX = (screenX + 1.0f) * 0.5f * viewportWidth;
            drawY = (1.0f - screenY) * 0.5f * viewportHeight;
        }

        QString qText = QString::fromStdString(item.text);

        // 背景框
        if (item.hasBackground)
        {
            QRectF textRect = fm.boundingRect(qText);
            float bgW = textRect.width() + item.bgPaddingX * 2;
            float bgH = textRect.height() + item.bgPaddingY * 2;

            float bgX = drawX, bgY = drawY;
            switch (item.hAlign)
            {
                case Render::UiTextHAlign::Center: bgX -= bgW * 0.5f; break;
                case Render::UiTextHAlign::Right:  bgX -= bgW; break;
                default: break;
            }
            switch (item.vAlign)
            {
                case Render::UiTextVAlign::Middle: bgY -= bgH * 0.5f; break;
                case Render::UiTextVAlign::Bottom: bgY -= bgH; break;
                default: break;
            }

            painter.setBrush(QColor::fromRgbF(item.bgColor.r(), item.bgColor.g(),
                item.bgColor.b(), item.bgColor.a()));
            painter.setPen(Qt::NoPen);
            if (item.bgRadius > 0.0f)
            {
                painter.drawRoundedRect(QRectF(bgX, bgY, bgW, bgH),
                    item.bgRadius, item.bgRadius);
            }
            else
            {
                painter.drawRect(QRectF(bgX, bgY, bgW, bgH));
            }
            painter.setPen(QColor::fromRgbF(item.color.r(), item.color.g(),
                item.color.b(), item.color.a()));
        }

        // 对齐计算
        int flags = Qt::AlignLeft | Qt::AlignTop;
        switch (item.hAlign)
        {
            case Render::UiTextHAlign::Center: flags = Qt::AlignHCenter | Qt::AlignTop; break;
            case Render::UiTextHAlign::Right:  flags = Qt::AlignRight  | Qt::AlignTop; break;
            default: break;
        }
        switch (item.vAlign)
        {
            case Render::UiTextVAlign::Middle: flags = (flags & ~Qt::AlignTop) | Qt::AlignVCenter; break;
            case Render::UiTextVAlign::Bottom: flags = (flags & ~Qt::AlignTop) | Qt::AlignBottom; break;
            default: break;
        }

        QRectF textRect(drawX, drawY, 0, 0);
        painter.drawText(textRect, flags, qText);
    }
}

void UnifiedTextRenderer::renderTexts(const Render::UiTextItemList& texts,
    QPainter* fallbackPainter,
    int viewportWidth, int viewportHeight,
    const Ut::Mat3f& viewMatrix)
{
    if (m_useGPU && m_gpuInitialized && m_textRenderer)
    {
        renderTextsGPU(texts);
    }
    else if (fallbackPainter)
    {
        renderTextsQPainter(*fallbackPainter, texts, viewportWidth, viewportHeight, viewMatrix);
    }
}
