#pragma once

#include "RenderAPI.h"
#include "GLVerDef.h"
#include "Render/RenderTypes.h"
#include "Ut/Mat.h"
#include <QOpenGLShaderProgram>

/**
 * @brief 渲染数据消费者
 *
 * 接收 RenderCommandList（Render 层稳定数据契约），转化为 OpenGL 绘制调用。
 * - 数据所有权明确，无悬垂指针风险
 * - 支持 per-vertex color 属性
 * - 所有顶点通过单一 VBO 上传，减少 GPU 调用次数
 */
class RENDER_API RenderDataConsumer : protected XGLFunctions
{
public:
    RenderDataConsumer();
    ~RenderDataConsumer();

public:
    // 初始化OpenGL资源
    bool initialize();

    // 清理OpenGL资源
    void cleanup();

    // 设置渲染参数
    void setViewMatrix(const Ut::Mat3f& matrix);
    void setDefaultColor(float r, float g, float b, float a = 1.0f);
    void setDefaultLineWidth(float width);

    /**
     * @brief 渲染命令列表
     * @param commands 渲染层命令列表
     */
    void render(const Render::RenderCommandList& commands);

private:
    // 渲染单个命令（内部使用）
    void renderCommand(const Render::RenderCommand& cmd);

    // 批处理渲染：合并同类型命令，减少 Draw Call
    void renderBatch(const std::vector<const Render::RenderCommand*>& batch);

    // 转换图元类型为OpenGL枚举
    GLenum toGLPrimitiveType(Render::RenderPrimitiveType type);

    // 确保VBO容量足够
    void ensureVBOCapacity(size_t vertexCount);

    // 判断两个命令是否可以合并批处理
    static bool canBatch(const Render::RenderCommand& a, const Render::RenderCommand& b);

private:
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    size_t m_vboCapacity = 0;

    // 渲染状态
    Ut::Mat3f m_viewMatrix;
    float m_defaultColor[4];
    float m_defaultLineWidth = 1.0f;

    bool m_bInitialized = false;
};
