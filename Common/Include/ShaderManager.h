#pragma once

#include "RenderAPI.h"
#include <QOpenGLShaderProgram>
#include <QMap>
#include <QString>

/**
 * @brief 统一着色器管理器
 *
 * 管理所有着色器程序的编译、链接和生命周期。
 * 替代旧版分散在 RenderWidget 和 RenderDataConsumer 中的重复着色器创建。
 *
 * 设计：
 * - 单例模式，整个Render模块共享
 * - 按名称注册着色器，避免重复编译
 * - 支持 uniform 位置缓存，避免每帧字符串查找
 */
class RENDER_API ShaderManager
{
public:
    static ShaderManager& instance();

    ShaderManager(const ShaderManager&) = delete;
    ShaderManager& operator=(const ShaderManager&) = delete;

    /**
     * @brief 初始化所有内置着色器
     * @param parent OpenGL上下文的父对象（用于着色器创建）
     * @return 是否全部成功
     */
    bool initialize(QObject* parent = nullptr);

    /**
     * @brief 释放所有着色器资源
     */
    void cleanup();

    /**
     * @brief 获取指定名称的着色器程序
     * @param name 着色器名称（如 "line", "flat", "bitmap"）
     * @return 着色器程序指针，不存在则返回nullptr
     */
    QOpenGLShaderProgram* get(const QString& name) const;

    /**
     * @brief 获取指定着色器的 uniform 位置（缓存）
     * @param shaderName 着色器名称
     * @param uniformName uniform 变量名
     * @return uniform 位置，-1表示不存在
     */
    GLint uniformLocation(const QString& shaderName, const QString& uniformName);

    // ==================== 便捷方法 ====================

    /// 获取主场景渲染着色器（支持 per-vertex color）
    QOpenGLShaderProgram* sceneShader() const
    {
        return get("scene");
    }

    /// 获取平面着色器（场景环境几何/标尺等）
    QOpenGLShaderProgram* flatShader() const
    {
        return get("flat");
    }

    /// 获取位图着色器
    QOpenGLShaderProgram* bitmapShader() const
    {
        return get("bitmap");
    }

private:
    ShaderManager() = default;
    ~ShaderManager();

    /// 编译并注册一个着色器
    bool compileAndRegister(const QString& name,
        const char* vsSource, const char* fsSource,
        QObject* parent);

private:
    QMap<QString, QOpenGLShaderProgram*> m_shaders;

    // uniform 位置缓存: shaderName -> (uniformName -> location)
    QMap<QString, QMap<QString, GLint>> m_uniformCache;

    bool m_initialized = false;
    bool m_cleanedUp = false;
};
