#pragma once

/// @brief 3D 渲染数据驱动管线 — 数据包
///
/// 设计原则（与 2D RenderCommandList 一致）：
/// - 数据所有权归 RenderPacket3D，不持有 Engine 对象指针
/// - Render 层只消费数据，不依赖 Engine 层
/// - Engine 层负责生成数据包，Render 层负责渲染

#include "RenderAPI.h"
#include <vector>
#include <cstdint>

namespace Render
{
    /// @brief 3D 渲染数据包 — 单个可渲染对象
    /// 包含完整的顶点数据、材质属性、变换矩阵，渲染层无需访问 Engine 对象
    struct RENDER_API RenderPacket3D
    {
        /// 交错顶点数据：position(3f) + normal(3f) = 6 floats/vertex
        std::vector<float> vertexData;
        uint64_t entityId = 0;  ///< 用于关联 Engine 实体（可选）

        /// 索引数据（可选，为空时使用 glDrawArrays）
        std::vector<unsigned int> indices;

        /// 材质属性
        float ambientColor[3]  = { 0.2f, 0.2f, 0.2f };
        float diffuseColor[3]  = { 0.7f, 0.7f, 0.7f };
        float specularColor[3] = { 0.3f, 0.3f, 0.3f };
        float shininess = 32.0f;

        /// 模型矩阵（4x4 列主序）
        float modelMatrix[16] = {
            1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
        };

        /// 高亮模式（用于选中高亮）
        bool isHighlighted = false;

        bool isValid() const { return !vertexData.empty() && vertexData.size() % 6 == 0; }
        size_t vertexCount() const { return vertexData.size() / 6; }
        size_t indexCount() const { return indices.size(); }
    };

    /// @brief 3D 渲染世界 — 完整场景渲染数据
    struct RENDER_API RenderWorld3D
    {
        /// 场景中的所有可渲染对象
        std::vector<RenderPacket3D> packets;

        /// 高亮实体（选中状态）
        std::vector<RenderPacket3D> highlightPackets;

        bool empty() const { return packets.empty() && highlightPackets.empty(); }
        void clear()
        {
            packets.clear();
            highlightPackets.clear();
        }
    };
} // namespace Render