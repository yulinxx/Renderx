#include "Render3D/SceneConverter3D.h"
#include "Engine3D/SceneManager3D.h"
#include "Engine3D/SyEntity/SyMeshEntity.h"
#include "Engine3D/Selection/SelectionManager3D.h"
#include "Ut/Mat.h"
#include <cstring>
#include <unordered_set>

namespace Render
{
    void SceneConverter3D::convertEntity(const Eg::SyMeshEntity* entity,
        RenderPacket3D& outPacket) const
    {
        if (!entity || entity->vertices.empty())
        {
            outPacket.vertexData.clear();
            outPacket.indices.clear();
            return;
        }

        // 交错顶点数据：position(3) + normal(3) = 6 floats/vertex
        size_t vCount = entity->vertices.size();
        outPacket.vertexData.resize(vCount * 6);
        outPacket.entityId = entity->id;

        for (size_t i = 0; i < vCount; ++i)
        {
            float* dst = outPacket.vertexData.data() + i * 6;
            dst[0] = entity->vertices[i][0];
            dst[1] = entity->vertices[i][1];
            dst[2] = entity->vertices[i][2];

            if (i < entity->normals.size())
            {
                dst[3] = entity->normals[i][0];
                dst[4] = entity->normals[i][1];
                dst[5] = entity->normals[i][2];
            }
            else
            {
                dst[3] = 0.0f;
                dst[4] = 1.0f;
                dst[5] = 0.0f;
            }
        }

        // 材质
        outPacket.ambientColor[0]  = entity->ambientColor[0];
        outPacket.ambientColor[1]  = entity->ambientColor[1];
        outPacket.ambientColor[2]  = entity->ambientColor[2];
        outPacket.diffuseColor[0]  = entity->diffuseColor[0];
        outPacket.diffuseColor[1]  = entity->diffuseColor[1];
        outPacket.diffuseColor[2]  = entity->diffuseColor[2];
        outPacket.specularColor[0] = entity->specularColor[0];
        outPacket.specularColor[1] = entity->specularColor[1];
        outPacket.specularColor[2] = entity->specularColor[2];
        outPacket.shininess = entity->shininess;
    }

    void SceneConverter3D::convertScene(const Eg::SceneManager3D* sceneManager,
        RenderWorld3D& outWorld)
    {
        outWorld.clear();
        if (!sceneManager) return;

        const auto& entities = sceneManager->getAllEntities();
        outWorld.packets.reserve(entities.size());

        for (const auto& entity : entities)
        {
            if (!entity || !entity->isValid()) continue;

            RenderPacket3D packet;
            convertEntity(entity.get(), packet);
            if (packet.isValid())
            {
                outWorld.packets.push_back(std::move(packet));
            }
        }
    }

    void SceneConverter3D::convertSceneWithSelection(
        const Eg::SceneManager3D* sceneManager,
        const Eg::SelectionManager3D* selectionManager,
        RenderWorld3D& outWorld)
    {
        outWorld.clear();
        if (!sceneManager) return;

        const auto& entities = sceneManager->getAllEntities();
        outWorld.packets.reserve(entities.size());

        // 获取选中实体集合（用于快速查找）
        std::unordered_set<const Eg::SyMeshEntity*> selectedSet;
        if (selectionManager && selectionManager->hasSelection())
        {
            const auto& selected = selectionManager->getSelectedEntities();
            for (const auto* s : selected)
            {
                if (s) selectedSet.insert(s);
            }
        }

        for (const auto& entity : entities)
        {
            if (!entity || !entity->isValid()) continue;

            const Eg::SyMeshEntity* rawPtr = entity.get();
            bool isSelected = selectedSet.count(rawPtr) > 0;

            RenderPacket3D packet;
            convertEntity(rawPtr, packet);

            // 变换中的实体应用变换矩阵
            if (isSelected && selectionManager && selectionManager->isTransforming())
            {
                Ut::Mat4f transform = selectionManager->getTransformMatrix();
                std::memcpy(packet.modelMatrix, transform.data, sizeof(float) * 16);
            }

            packet.isHighlighted = isSelected;

            if (packet.isValid())
            {
                if (isSelected)
                {
                    outWorld.highlightPackets.push_back(std::move(packet));
                }
                else
                {
                    outWorld.packets.push_back(std::move(packet));
                }
            }
        }
    }
} // namespace Render