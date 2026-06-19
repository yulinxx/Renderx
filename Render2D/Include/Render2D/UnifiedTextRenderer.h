#pragma once

/// @brief 统一文本渲染桥接器
///
/// 解决 TextRenderer (GPU) 与 QPainter (CPU) 双路径问题。
/// 优先使用 GPU 文本渲染，仅在不可用时回退到 QPainter。
///
/// 用法：
///   1. 在 initializeGL() 中调用 initialize()
///   2. 在 paintGL() 中调用 beginFrame() / renderTexts() / endFrame()
///   3. 保留 QPainter 回退，通过 setUseGPU(true/false) 切换

#include "RenderAPI.h"
#include "Render/RenderTypes.h"
#include <vector>
#include <string>

namespace Rd
{
    class TextRenderer;
}

class QPainter;

/**
 * @brief 统一文本渲染器
 *
 * 整合 TextRenderer (GPU 字图集) 和 QPainter (CPU 绘制)，
 * 提供统一的文本渲染 API。优先使用 GPU 路径以获得更好的性能。
 */
class RENDER_API UnifiedTextRenderer
{
public:
    UnifiedTextRenderer();
    ~UnifiedTextRenderer();

    /// 初始化 GPU 文本渲染器（在 initializeGL 中调用）
    bool initializeGPU();

    /// 清理 GPU 资源
    void cleanupGPU();

    /// 设置是否使用 GPU 文本渲染（默认 true）
    void setUseGPU(bool useGPU) { m_useGPU = useGPU; }
    bool isUsingGPU() const { return m_useGPU; }

    /// 开始帧（GPU 模式下调用）
    void beginFrame(int viewportWidth, int viewportHeight, const Ut::Mat3f& viewMatrix);

    /// 使用 GPU 渲染文本列表
    void renderTextsGPU(const Render::UiTextItemList& texts);

    /// 使用 QPainter 渲染文本列表（回退路径）
    void renderTextsQPainter(QPainter& painter, const Render::UiTextItemList& texts,
        int viewportWidth, int viewportHeight, const Ut::Mat3f& viewMatrix);

    /// 统一入口：自动选择 GPU 或 QPainter
    void renderTexts(const Render::UiTextItemList& texts,
        QPainter* fallbackPainter,
        int viewportWidth, int viewportHeight,
        const Ut::Mat3f& viewMatrix);

    /// 结束帧（GPU 模式下调用）
    void endFrame();

private:
    Rd::TextRenderer* m_textRenderer = nullptr;
    bool m_useGPU = true;
    bool m_gpuInitialized = false;
};
