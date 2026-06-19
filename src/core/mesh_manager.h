#pragma once

#include "render/render_types.h"
#include "../rhi/rhi_device.h"
#include "../core/slot_map.h"
#include <vector>
#include <cstdint>

namespace render::core {

class MeshManager {
public:
    bool initialize(rhi::IDevice* device);
    void shutdown();

    MeshId registerMesh(const float* positions, const float* normals,
                        const uint32_t* indices, uint32_t vertexCount, uint32_t indexCount);
    void unregisterMesh(MeshId mesh);

    uint32_t addInstance(MeshId mesh, const float modelMatrix[16], uint32_t materialIdx);
    void modifyInstance(uint32_t instanceId, const float modelMatrix[16]);
    void removeInstance(uint32_t instanceId);
    void setInstanceVisibility(uint32_t instanceId, bool visible);

    void queryVisible(const float viewMatrix[16], const float projMatrix[16],
                      uint32_t* outInstanceIds, uint32_t* outCount, uint32_t maxOut);

    void update();

    void render(rhi::IDevice* device, const float viewMatrix[16], const float projMatrix[16]);

private:
    struct MeshEntry {
        uint32_t indexOffset;
        uint32_t indexCount;
        uint32_t vertexOffset;
        uint32_t vertexCount;
        float bbox[6];
        bool deleted;
    };

    struct InstanceEntry {
        float modelMatrix[16];
        uint32_t meshDenseIdx;
        uint32_t materialIndex;
        uint32_t flags;
        bool dirty;
    };

    SlotMap<uint64_t, MeshEntry> m_meshes;
    std::vector<VertexP3N3> m_positions;
    std::vector<uint32_t> m_indices;

    std::vector<InstanceEntry> m_instances;
    std::vector<uint32_t> m_dirtyInstances;
    std::vector<uint32_t> m_freeInstances;

    rhi::IDevice* m_device = nullptr;
    rhi::BufferHandle m_positionBuffer = rhi::NullHandle;
    rhi::BufferHandle m_indexBuffer = rhi::NullHandle;
    rhi::BufferHandle m_instanceBuffer = rhi::NullHandle;
    rhi::PipelineHandle m_meshPipeline = rhi::NullHandle;
    rhi::PipelineHandle m_wireframePipeline = rhi::NullHandle;
    rhi::PipelineHandle m_highlightPipeline = rhi::NullHandle;
    bool m_meshBufferDirty = false;
    bool m_instanceBufferDirty = false;

    std::vector<uint32_t> m_visibleInstances;

    void buildPipelines(rhi::IDevice* device);
    void uploadMeshBuffers(rhi::IDevice* device);
    void uploadInstanceBuffer(rhi::IDevice* device);
};

}
