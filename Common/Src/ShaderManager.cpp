#include "ShaderManager.h"
#include "ShaderDef.h"
#include "GLVerDef.h"
#include "Log/SyLogger.h"
#include <QDebug>

ShaderManager& ShaderManager::instance()
{
    static ShaderManager s_instance;
    return s_instance;
}

ShaderManager::~ShaderManager()
{
    // ⚠️ 重要：这里是 C++ 静态对象销毁阶段（main() 已返回）
    // 此时 Qt 的 OpenGL context 已经被销毁
    // 所以绝对不能在这个析构函数中调用任何 GL 函数或 delete QOpenGLShaderProgram
    // （QOpenGLShaderProgram 的析构函数会调用 glDeleteProgram）
    //
    // 正确的 cleanup 时机：
    //   1. RenderWidget::~RenderWidget() 中显式调用 ShaderManager::instance().cleanup()
    //   2. 或者在 main() 返回前（有 context 时）显式调用
    //
    // 这里设置 m_cleanedUp = true 来阻止 cleanup() 被意外调用
    m_cleanedUp = true;

    // 对于还未被清理的 shader，我们只能"放弃"它们（leak them）
    // 因为进程即将退出，操作系统会回收所有 GPU 资源
    // 这比在无 context 下调用 glDeleteProgram 导致崩溃要好得多
    // 注意：我们不调用 delete m_shaders[...] 来避免触发 GL 调用
    m_shaders.clear();  // 只清空容器（不 delete），让操作系统在进程退出时回收

    m_initialized = false;
}

bool ShaderManager::initialize(QObject* parent)
{
    if (m_initialized)
        return true;

    // 主场景着色器 — 支持 per-vertex color
    // 顶点输入: position(vec2) + color(vec3)
    // uniform: projectionView(mat3), uniformColor(vec4), useVertexColor(bool)
    static const char* SCENE_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        layout(location = 1) in vec3 vertexColor;
        uniform mat3 projectionView;
        out vec3 vColor;
        void main()
        {
            vec3 clipPos = projectionView * vec3(position, 1.0);
            gl_Position = vec4(clipPos.xy, 0.0, 1.0);
            vColor = vertexColor;
        }
    )";
    static const char* SCENE_FS = GLSL_VERSION_STR R"(
        uniform vec4 uniformColor;
        uniform bool useVertexColor;
        in vec3 vColor;
        out vec4 fragColor;
        void main()
        {
            if (useVertexColor)
                fragColor = vec4(vColor, 1.0);
            else
                fragColor = uniformColor;
        }
    )";

    // 平面着色器 — 纯 uniform color（场景环境几何/标尺/网格）
    // 顶点输入: position(vec2)
    static const char* FLAT_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 screenToNdc;
        uniform float zDepth;
        void main()
        {
            vec3 clip = screenToNdc * vec3(position, 1.0);
            gl_Position = vec4(clip.xy, zDepth, 1.0);
        }
    )";
    static const char* FLAT_FS = GLSL_VERSION_STR R"(
        uniform vec4 color;
        out vec4 fragColor;
        void main()
        {
            fragColor = color;
        }
    )";

    // 位图着色器
    static const char* BITMAP_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        uniform mat3 projectionView;
        void main()
        {
            vUV = aUV;
            vec3 p = projectionView * vec3(aPos.xy, 1.0);
            gl_Position = vec4(p.xy, 0.0, 1.0);
        }
    )";
    static const char* BITMAP_FS = GLSL_VERSION_STR R"(
        in vec2 vUV;
        out vec4 FragColor;
        uniform sampler2D uTex;
        void main()
        {
            FragColor = texture(uTex, vUV);
        }
    )";

    // 十字光标着色器
    static const char* CROSS_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 projectionView;
        uniform vec2 mousePos;
        uniform vec2 viewportSize;
        void main()
        {
            vec3 mouseClipPos = projectionView * vec3(mousePos, 1.0);
            float crossPixels = length(position);
            float normalizeFactor = (crossPixels > 0.0) ? crossPixels / (min(viewportSize.x, viewportSize.y) * 0.5) : 0.0;
            vec2 screenOffset = normalize(position) * normalizeFactor;
            gl_Position = vec4(mouseClipPos.xy + screenOffset, 0.0, 1.0);
        }
    )";
    static const char* CROSS_FS = GLSL_VERSION_STR R"(
        out vec4 fragColor;
        void main()
        {
            fragColor = vec4(0.0, 0.0, 1.0, 1.0);
        }
    )";

    // 控制线着色器（与line相同结构）
    static const char* CONTROL_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 projectionView;
        void main()
        {
            vec3 clipPos = projectionView * vec3(position, 1.0);
            gl_Position = vec4(clipPos.xy, 0.0, 1.0);
        }
    )";
    static const char* CONTROL_FS = GLSL_VERSION_STR R"(
        uniform vec4 color;
        out vec4 fragColor;
        void main()
        {
            fragColor = color;
        }
    )";

    // 捕捉标记着色器
    static const char* SNAP_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 projectionView;
        uniform vec2 snapWorldPos;
        uniform vec2 viewportSize;
        uniform float markerSize;
        void main()
        {
            vec3 snapClip = projectionView * vec3(snapWorldPos, 1.0);
            float px = abs(position.x) > 0.001 ? position.x : 0.0;
            float py = abs(position.y) > 0.001 ? position.y : 0.0;
            float len = length(vec2(px, py));
            float scale = (len > 0.001) ? (markerSize / len) / min(viewportSize.x, viewportSize.y) : 0.0;
            vec2 screenOffset = position * scale;
            gl_Position = vec4(snapClip.xy + screenOffset, 0.0, 1.0);
        }
    )";
    static const char* SNAP_FS = GLSL_VERSION_STR R"(
        uniform vec4 markerColor;
        out vec4 fragColor;
        void main()
        {
            fragColor = markerColor;
        }
    )";

    // 点标记着色器
    static const char* POINT_MARKER_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 projectionView;
        uniform vec2 viewportSize;
        uniform float markerSize;
        uniform int cornerIndex;
        void main()
        {
            vec3 clip = projectionView * vec3(position, 1.0);
            int id = gl_VertexID - (gl_VertexID / 4) * 4;
            vec2 corner;
            if (id == 0)      corner = vec2(-1.0, -1.0);
            else if (id == 1) corner = vec2( 1.0, -1.0);
            else if (id == 2) corner = vec2( 1.0,  1.0);
            else               corner = vec2(-1.0,  1.0);
            vec2 ndcOffset = corner * (markerSize / viewportSize);
            gl_Position = vec4(clip.xy + ndcOffset, 0.0, 1.0);
        }
    )";
    static const char* POINT_MARKER_FS = GLSL_VERSION_STR R"(
        uniform vec4 fillColor;
        out vec4 fragColor;
        void main()
        {
            fragColor = fillColor;
        }
    )";

    // 注册所有着色器
    bool ok = true;
    ok &= compileAndRegister("scene", SCENE_VS, SCENE_FS, parent);
    ok &= compileAndRegister("flat", FLAT_VS, FLAT_FS, parent);
    ok &= compileAndRegister("bitmap", BITMAP_VS, BITMAP_FS, parent);
    ok &= compileAndRegister("cross", CROSS_VS, CROSS_FS, parent);
    ok &= compileAndRegister("line", CONTROL_VS, CONTROL_FS, parent);     // line 与 control 共用着色器源码
    ok &= compileAndRegister("control", CONTROL_VS, CONTROL_FS, parent);
    ok &= compileAndRegister("snap", SNAP_VS, SNAP_FS, parent);
    ok &= compileAndRegister("pointMarker", POINT_MARKER_VS, POINT_MARKER_FS, parent);

    m_initialized = ok;
    return ok;
}

void ShaderManager::cleanup()
{
    if (m_cleanedUp) return;

    for (auto it = m_shaders.begin(); it != m_shaders.end(); ++it)
    {
        delete it.value();
    }
    m_shaders.clear();
    m_uniformCache.clear();
    m_initialized = false;
    m_cleanedUp = true;
}

QOpenGLShaderProgram* ShaderManager::get(const QString& name) const
{
    auto it = m_shaders.find(name);
    return (it != m_shaders.end()) ? it.value() : nullptr;
}

GLint ShaderManager::uniformLocation(const QString& shaderName, const QString& uniformName)
{
    // 检查缓存
    auto shaderCache = m_uniformCache.find(shaderName);
    if (shaderCache != m_uniformCache.end())
    {
        auto locIt = shaderCache->find(uniformName);
        if (locIt != shaderCache->end())
            return locIt.value();
    }

    // 查找着色器
    QOpenGLShaderProgram* shader = get(shaderName);
    if (!shader)
        return -1;

    GLint loc = shader->uniformLocation(uniformName);
    m_uniformCache[shaderName][uniformName] = loc;
    return loc;
}

bool ShaderManager::compileAndRegister(const QString& name,
    const char* vsSource, const char* fsSource,
    QObject* parent)
{
    if (m_shaders.contains(name))
        return true;

    auto* program = new QOpenGLShaderProgram(nullptr);  // no parent, ShaderManager owns lifecycle

    if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vsSource))
    {
        SY_ERRORF("[ShaderManager] Failed to compile vertex shader '%s': %s", qPrintable(name), qPrintable(program->log()));
        delete program;
        return false;
    }

    if (!program->addShaderFromSourceCode(QOpenGLShader::Fragment, fsSource))
    {
        SY_ERRORF("[ShaderManager] Failed to compile fragment shader '%s': %s", qPrintable(name), qPrintable(program->log()));
        delete program;
        return false;
    }

    if (!program->link())
    {
        SY_ERRORF("[ShaderManager] Failed to link shader '%s': %s", qPrintable(name), qPrintable(program->log()));
        delete program;
        return false;
    }

    m_shaders[name] = program;
    SY_INFOF("[ShaderManager] Shader '%s' compiled and registered", qPrintable(name));
    return true;
}