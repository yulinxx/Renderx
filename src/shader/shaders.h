/**
 * @file shaders.h
 * @brief Shader 资源管理接口
 *
 * 提供从文件加载 GLSL shader 源码的功能，并将其存储为全局字符串指针。
 * 所有 shader 在运行时从文件加载，便于开发和调试。
 */
#pragma once

#include <string>

namespace render::shader
{

    /// 2D场景渲染顶点着色器
    extern const char* SCENE_2D_VERT;
    /// 2D场景渲染片段着色器
    extern const char* SCENE_2D_FRAG;
    /// 叠加层顶点着色器（世界坐标）
    extern const char* OVERLAY_VERT;
    /// 叠加层片段着色器
    extern const char* OVERLAY_FRAG;
    /// 屏幕坐标叠加层顶点着色器
    extern const char* OVERLAY_SCREEN_VERT;
    /// 屏幕坐标叠加层片段着色器
    extern const char* OVERLAY_SCREEN_FRAG;
    /// 位图渲染顶点着色器
    extern const char* BITMAP_VERT;
    /// 位图渲染片段着色器
    extern const char* BITMAP_FRAG;
    /// 3D网格渲染顶点着色器
    extern const char* MESH_3D_VERT;
    /// 3D网格渲染片段着色器
    extern const char* MESH_3D_FRAG;
    /// 3D网格实例化渲染顶点着色器
    extern const char* MESH_3D_INSTANCED_VERT;
    /// SDF文本渲染顶点着色器
    extern const char* TEXT_SDF_VERT;
    /// SDF文本渲染片段着色器
    extern const char* TEXT_SDF_FRAG;
    /// 屏幕空间文本渲染顶点着色器
    extern const char* TEXT_SCREEN_VERT;
    /// 屏幕空间文本渲染片段着色器
    extern const char* TEXT_SCREEN_FRAG;
    /// 高亮渲染顶点着色器
    extern const char* HIGHLIGHT_3D_VERT;
    /// 高亮渲染片段着色器
    extern const char* HIGHLIGHT_3D_FRAG;
    /// GPU 剔除计算着色器
    extern const char* CULLING_COMP;

    /**
     * @brief 初始化 shader 系统
     *
     * 从指定目录加载所有 shader 文件。
     *
     * @param shaderDir shader 文件所在目录
     */
    void initialize(const std::string& shaderDir);

}  // namespace render::shader
