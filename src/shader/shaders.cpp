/**
 * @file shaders.cpp
 * @brief Shader 源码加载与管理
 *
 * @warning 当前 shader 源码使用文件级静态变量存储，属于进程级共享资源。
 *          这意味着所有窗口和会话共享同一份 shader 源码，不是 per-window 独立资源。
 *          后续应迁移到 RenderRuntime 管理，以支持多窗口独立 shader 主题和热更新。
 *          （M1 已开始迁移：RenderRuntime::initialize() 目前仍调用此模块初始化）
 */
#include "shader/shaders.h"

#include <string>
#include <fstream>
#include <sstream>
#include "Log/SyLogger.h"

namespace render
{
    namespace shader
    {
        static std::string scene2dVertSource;
        static std::string scene2dFragSource;
        static std::string overlayVertSource;
        static std::string overlayFragSource;
        static std::string overlayScreenVertSource;
        static std::string overlayScreenFragSource;
        static std::string bitmapVertSource;
        static std::string bitmapFragSource;
        static std::string mesh3dVertSource;
        static std::string mesh3dFragSource;
        static std::string mesh3dInstancedVertSource;
        static std::string textSdfVertSource;
        static std::string textSdfFragSource;
        static std::string textScreenVertSource;
        static std::string textScreenFragSource;
        static std::string highlight3dVertSource;
        static std::string highlight3dFragSource;
        static std::string cullingCompSource;

        const char* SCENE_2D_VERT = nullptr;
        const char* SCENE_2D_FRAG = nullptr;
        const char* OVERLAY_VERT = nullptr;
        const char* OVERLAY_FRAG = nullptr;
        const char* OVERLAY_SCREEN_VERT = nullptr;
        const char* OVERLAY_SCREEN_FRAG = nullptr;
        const char* BITMAP_VERT = nullptr;
        const char* BITMAP_FRAG = nullptr;
        const char* MESH_3D_VERT = nullptr;
        const char* MESH_3D_FRAG = nullptr;
        const char* MESH_3D_INSTANCED_VERT = nullptr;
        const char* TEXT_SDF_VERT = nullptr;
        const char* TEXT_SDF_FRAG = nullptr;
        const char* TEXT_SCREEN_VERT = nullptr;
        const char* TEXT_SCREEN_FRAG = nullptr;
        const char* HIGHLIGHT_3D_VERT = nullptr;
        const char* HIGHLIGHT_3D_FRAG = nullptr;
        const char* CULLING_COMP = nullptr;

        static bool loadShaderFile(const std::string& dir, const std::string& fileName, std::string& outSource)
        {
            std::string filePath = dir + "/" + fileName;
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                SY_WARNF("[Shader] Failed to open shader file: %s", filePath.c_str());
                return false;
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            outSource = buffer.str();
            // SY_DEBUGF("[Shader] Loaded shader file: %s (size=%zu bytes)", filePath.c_str(), outSource.size());
            return true;
        }

        void initialize(const std::string& shaderDir)
        {
            // SY_INFO("[Shader] Initializing shader system");
            // SY_INFOF("[Shader] Shader directory: %s", shaderDir.c_str());

            loadShaderFile(shaderDir, "scene_2d.vert", scene2dVertSource);
            loadShaderFile(shaderDir, "scene_2d.frag", scene2dFragSource);
            loadShaderFile(shaderDir, "overlay.vert", overlayVertSource);
            loadShaderFile(shaderDir, "overlay.frag", overlayFragSource);
            loadShaderFile(shaderDir, "overlay_screen.vert", overlayScreenVertSource);
            loadShaderFile(shaderDir, "overlay_screen.frag", overlayScreenFragSource);
            loadShaderFile(shaderDir, "bitmap.vert", bitmapVertSource);
            loadShaderFile(shaderDir, "bitmap.frag", bitmapFragSource);
            loadShaderFile(shaderDir, "mesh_3d.vert", mesh3dVertSource);
            loadShaderFile(shaderDir, "mesh_3d.frag", mesh3dFragSource);
            loadShaderFile(shaderDir, "mesh_3d_instanced.vert", mesh3dInstancedVertSource);
            loadShaderFile(shaderDir, "text_sdf.vert", textSdfVertSource);
            loadShaderFile(shaderDir, "text_sdf.frag", textSdfFragSource);
            loadShaderFile(shaderDir, "text_screen.vert", textScreenVertSource);
            loadShaderFile(shaderDir, "text_screen.frag", textScreenFragSource);
            loadShaderFile(shaderDir, "highlight_3d.vert", highlight3dVertSource);
            loadShaderFile(shaderDir, "highlight_3d.frag", highlight3dFragSource);
            loadShaderFile(shaderDir, "culling.comp", cullingCompSource);

            SCENE_2D_VERT = scene2dVertSource.empty() ? "" : scene2dVertSource.c_str();
            SCENE_2D_FRAG = scene2dFragSource.empty() ? "" : scene2dFragSource.c_str();
            OVERLAY_VERT = overlayVertSource.empty() ? "" : overlayVertSource.c_str();
            OVERLAY_FRAG = overlayFragSource.empty() ? "" : overlayFragSource.c_str();
            OVERLAY_SCREEN_VERT = overlayScreenVertSource.empty() ? "" : overlayScreenVertSource.c_str();
            OVERLAY_SCREEN_FRAG = overlayScreenFragSource.empty() ? "" : overlayScreenFragSource.c_str();
            BITMAP_VERT = bitmapVertSource.empty() ? "" : bitmapVertSource.c_str();
            BITMAP_FRAG = bitmapFragSource.empty() ? "" : bitmapFragSource.c_str();
            MESH_3D_VERT = mesh3dVertSource.empty() ? "" : mesh3dVertSource.c_str();
            MESH_3D_FRAG = mesh3dFragSource.empty() ? "" : mesh3dFragSource.c_str();
            MESH_3D_INSTANCED_VERT = mesh3dInstancedVertSource.empty() ? "" : mesh3dInstancedVertSource.c_str();
            TEXT_SDF_VERT = textSdfVertSource.empty() ? "" : textSdfVertSource.c_str();
            TEXT_SDF_FRAG = textSdfFragSource.empty() ? "" : textSdfFragSource.c_str();
            TEXT_SCREEN_VERT = textScreenVertSource.empty() ? "" : textScreenVertSource.c_str();
            TEXT_SCREEN_FRAG = textScreenFragSource.empty() ? "" : textScreenFragSource.c_str();
            HIGHLIGHT_3D_VERT = highlight3dVertSource.empty() ? "" : highlight3dVertSource.c_str();
            HIGHLIGHT_3D_FRAG = highlight3dFragSource.empty() ? "" : highlight3dFragSource.c_str();
            CULLING_COMP = cullingCompSource.empty() ? "" : cullingCompSource.c_str();
        }
    }  // namespace shader
}  // namespace render