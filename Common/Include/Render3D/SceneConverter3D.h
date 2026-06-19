#pragma once

/// @brief 场景数据转换器 — Engine → Render 数据驱动桥接
///
/// 职责：将 Engine 层的 SceneManager3D/SyMeshEntity 转换为
/// Render 层的 RenderPacket3D/RenderWorld3D 数据包。
/// 此转换器属于 Engine 层，是分层架构的桥梁。

#include "Engine/EngineAPI.h"
#include "Render3D/RenderPacket3D.h"
#include <memory>

namespace Eg
{
    class SceneManager3D;
    class SelectionManager3D;
    struct SyMeshEntity;
}

namespace Render
{
    /// @brief 3D 场景数据转换器
    /// 将 Engine 层数据转换为 Render 层可消费的 RenderPacket3D
    class RENDER_API SceneConverter3D
    {
    public:
        SceneConverter3D() = default;
        ~SceneConverter3D() = default;

        /// @brief 从场景管理器生成渲染数据包
        /// @param sceneManager 场景数据源
        /// @param outWorld 输出的渲染世界数据
        void convertScene(const Eg::SceneManager3D* sceneManager,
            RenderWorld3D& outWorld);

        /// @brief 从场景管理器 + 选择管理器生成渲染数据包（含高亮）
        /// @param sceneManager 场景数据源
        /// @param selectionManager 选择状态（可为 nullptr）
        /// @param outWorld 输出的渲染世界数据
        void convertSceneWithSelection(
            const Eg::SceneManager3D* sceneManager,
            const Eg::SelectionManager3D* selectionManager,
            RenderWorld3D& outWorld);

    private:
        /// @brief 将单个网格实体转换为渲染数据包
        void convertEntity(const Eg::SyMeshEntity* entity,
            RenderPacket3D& outPacket) const;
    };
} // namespace Render