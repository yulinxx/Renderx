#include "mesh_manager.h"
#include "../shader/shaders.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace render {
namespace core {

namespace {

void mat4Multiply(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            out[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

void mat4TransformPoint(const float m[16], const float p[3], float out[3]) {
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

}

bool MeshManager::initialize(rhi::IDevice* device) {
    if (!device) return false;
    m_device = device;
    buildPipelines(device);
    return true;
}

void MeshManager::shutdown() {
    m_positions.clear();
    m_indices.clear();
    m_instances.clear();
    m_dirtyInstances.clear();
    m_freeInstances.clear();
    m_visibleInstances.clear();
    m_meshes.clear();

    if (m_positionBuffer != rhi::NullHandle) {
        m_device->destroyBuffer(m_positionBuffer);
        m_positionBuffer = rhi::NullHandle;
    }
    if (m_indexBuffer != rhi::NullHandle) {
        m_device->destroyBuffer(m_indexBuffer);
        m_indexBuffer = rhi::NullHandle;
    }
    if (m_instanceBuffer != rhi::NullHandle) {
        m_device->destroyBuffer(m_instanceBuffer);
        m_instanceBuffer = rhi::NullHandle;
    }
    if (m_meshPipeline != rhi::NullHandle) {
        m_device->destroyPipeline(m_meshPipeline);
        m_meshPipeline = rhi::NullHandle;
    }
    if (m_wireframePipeline != rhi::NullHandle) {
        m_device->destroyPipeline(m_wireframePipeline);
        m_wireframePipeline = rhi::NullHandle;
    }
    if (m_highlightPipeline != rhi::NullHandle) {
        m_device->destroyPipeline(m_highlightPipeline);
        m_highlightPipeline = rhi::NullHandle;
    }

    m_device = nullptr;
}

void MeshManager::buildPipelines(rhi::IDevice* device) {
    {
        rhi::PipelineDesc desc;
        desc.topology       = rhi::PrimitiveTopology::TriangleList;
        desc.vertexShader   = shader::MESH_3D_INSTANCED_VERT;
        desc.fragmentShader = shader::MESH_3D_FRAG;
        desc.computeShader  = nullptr;
        desc.depthTest      = true;
        desc.depthWrite     = true;
        desc.blendEnable    = false;
        desc.srcBlend       = rhi::BlendFactor::One;
        desc.dstBlend       = rhi::BlendFactor::Zero;
        desc.depthFunc      = rhi::CompareFunc::LessEqual;
        m_meshPipeline = device->createPipeline(desc);
    }

    {
        rhi::PipelineDesc desc;
        desc.topology       = rhi::PrimitiveTopology::TriangleList;
        desc.vertexShader   = shader::MESH_3D_INSTANCED_VERT;
        desc.fragmentShader = shader::MESH_3D_FRAG;
        desc.computeShader  = nullptr;
        desc.depthTest      = true;
        desc.depthWrite     = true;
        desc.blendEnable    = false;
        desc.srcBlend       = rhi::BlendFactor::One;
        desc.dstBlend       = rhi::BlendFactor::Zero;
        desc.depthFunc      = rhi::CompareFunc::LessEqual;
        m_wireframePipeline = device->createPipeline(desc);
    }

    {
        rhi::PipelineDesc desc;
        desc.topology       = rhi::PrimitiveTopology::TriangleList;
        desc.vertexShader   = shader::HIGHLIGHT_3D_VERT;
        desc.fragmentShader = shader::HIGHLIGHT_3D_FRAG;
        desc.computeShader  = nullptr;
        desc.depthTest      = true;
        desc.depthWrite     = false;
        desc.blendEnable    = true;
        desc.srcBlend       = rhi::BlendFactor::SrcAlpha;
        desc.dstBlend       = rhi::BlendFactor::OneMinusSrcAlpha;
        desc.depthFunc      = rhi::CompareFunc::LessEqual;
        m_highlightPipeline = device->createPipeline(desc);
    }
}

void MeshManager::uploadMeshBuffers(rhi::IDevice* device) {
    if (m_positions.empty() && m_indices.empty()) return;

    if (m_positionBuffer != rhi::NullHandle)
        device->destroyBuffer(m_positionBuffer);
    if (m_indexBuffer != rhi::NullHandle)
        device->destroyBuffer(m_indexBuffer);

    {
        rhi::BufferDesc desc;
        desc.size       = m_positions.size() * sizeof(VertexP3N3);
        desc.usage      = rhi::BufferUsage::Vertex;
        desc.memory     = rhi::MemoryType::GPU_Only;
        desc.debugName  = "MeshManager_PositionBuffer";
        m_positionBuffer = device->createBuffer(desc);
        device->uploadBuffer(m_positionBuffer, 0,
                             m_positions.size() * sizeof(VertexP3N3),
                             m_positions.data());
    }

    {
        rhi::BufferDesc desc;
        desc.size       = m_indices.size() * sizeof(uint32_t);
        desc.usage      = rhi::BufferUsage::Index;
        desc.memory     = rhi::MemoryType::GPU_Only;
        desc.debugName  = "MeshManager_IndexBuffer";
        m_indexBuffer = device->createBuffer(desc);
        device->uploadBuffer(m_indexBuffer, 0,
                             m_indices.size() * sizeof(uint32_t),
                             m_indices.data());
    }

    m_meshBufferDirty = false;
}

void MeshManager::uploadInstanceBuffer(rhi::IDevice* device) {
    if (m_instances.empty()) return;

    uint64_t requiredSize = m_instances.size() * sizeof(InstanceDesc);
    if (m_instanceBuffer != rhi::NullHandle) {
        device->destroyBuffer(m_instanceBuffer);
        m_instanceBuffer = rhi::NullHandle;
    }

    rhi::BufferDesc desc;
    desc.size       = requiredSize;
    desc.usage      = rhi::BufferUsage::ShaderVisible;
    desc.memory     = rhi::MemoryType::GPU_Only;
    desc.debugName  = "MeshManager_InstanceBuffer";
    m_instanceBuffer = device->createBuffer(desc);

    std::vector<InstanceDesc> instanceData(m_instances.size());
    for (uint32_t i = 0; i < m_instances.size(); ++i) {
        std::memcpy(instanceData[i].modelMatrix, m_instances[i].modelMatrix, 64);
        instanceData[i].meshIndex     = m_instances[i].meshDenseIdx;
        instanceData[i].materialIndex = m_instances[i].materialIndex;
        instanceData[i].flags         = m_instances[i].flags;
        instanceData[i]._pad          = 0;
    }

    device->uploadBuffer(m_instanceBuffer, 0, requiredSize, instanceData.data());
    m_instanceBufferDirty = false;
}

MeshId MeshManager::registerMesh(const float* positions, const float* normals,
                                  const uint32_t* indices, uint32_t vertexCount,
                                  uint32_t indexCount) {
    MeshEntry entry;
    entry.vertexOffset = static_cast<uint32_t>(m_positions.size());
    entry.indexOffset  = static_cast<uint32_t>(m_indices.size());
    entry.vertexCount  = vertexCount;
    entry.indexCount   = indexCount;
    entry.deleted      = false;

    float minX = 1e30f, minY = 1e30f, minZ = 1e30f;
    float maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;

    for (uint32_t i = 0; i < vertexCount; ++i) {
        VertexP3N3 v;
        v.px = positions[i * 3 + 0];
        v.py = positions[i * 3 + 1];
        v.pz = positions[i * 3 + 2];
        v.nx = normals[i * 3 + 0];
        v.ny = normals[i * 3 + 1];
        v.nz = normals[i * 3 + 2];
        m_positions.push_back(v);

        if (v.px < minX) minX = v.px;
        if (v.py < minY) minY = v.py;
        if (v.pz < minZ) minZ = v.pz;
        if (v.px > maxX) maxX = v.px;
        if (v.py > maxY) maxY = v.py;
        if (v.pz > maxZ) maxZ = v.pz;
    }

    entry.bbox[0] = minX;
    entry.bbox[1] = minY;
    entry.bbox[2] = minZ;
    entry.bbox[3] = maxX;
    entry.bbox[4] = maxY;
    entry.bbox[5] = maxZ;

    uint32_t vertexOffset = entry.vertexOffset;
    for (uint32_t i = 0; i < indexCount; ++i) {
        m_indices.push_back(indices[i] + vertexOffset);
    }

    MeshId id = m_meshes.insert(std::move(entry));
    m_meshBufferDirty = true;
    return id;
}

void MeshManager::unregisterMesh(MeshId mesh) {
    MeshEntry* entry = m_meshes.find(mesh);
    if (entry) entry->deleted = true;
}

uint32_t MeshManager::addInstance(MeshId mesh, const float modelMatrix[16],
                                   uint32_t materialIdx) {
    MeshEntry* entry = m_meshes.find(mesh);
    if (!entry || entry->deleted) return UINT32_MAX;

    uint32_t denseIdx = 0;
    const MeshEntry* denseData = m_meshes.dense_data();
    uint32_t denseCount = m_meshes.size();
    for (uint32_t i = 0; i < denseCount; ++i) {
        if (&denseData[i] == entry) {
            denseIdx = i;
            break;
        }
    }

    InstanceEntry inst;
    std::memcpy(inst.modelMatrix, modelMatrix, 64);
    inst.meshDenseIdx = denseIdx;
    inst.materialIndex = materialIdx;
    inst.flags = 1;
    inst.dirty = true;

    uint32_t id;
    if (!m_freeInstances.empty()) {
        id = m_freeInstances.back();
        m_freeInstances.pop_back();
        m_instances[id] = inst;
    } else {
        id = static_cast<uint32_t>(m_instances.size());
        m_instances.push_back(inst);
    }

    m_instanceBufferDirty = true;
    m_dirtyInstances.push_back(id);
    return id;
}

void MeshManager::modifyInstance(uint32_t instanceId, const float modelMatrix[16]) {
    if (instanceId >= m_instances.size()) return;
    std::memcpy(m_instances[instanceId].modelMatrix, modelMatrix, 64);
    m_instances[instanceId].dirty = true;
    m_instanceBufferDirty = true;
    m_dirtyInstances.push_back(instanceId);
}

void MeshManager::removeInstance(uint32_t instanceId) {
    if (instanceId >= m_instances.size()) return;
    m_instances[instanceId].flags = 0;
    m_instances[instanceId].meshDenseIdx = UINT32_MAX;
    m_instanceBufferDirty = true;
    m_freeInstances.push_back(instanceId);
}

void MeshManager::setInstanceVisibility(uint32_t instanceId, bool visible) {
    if (instanceId >= m_instances.size()) return;
    if (visible)
        m_instances[instanceId].flags |= 1u;
    else
        m_instances[instanceId].flags &= ~1u;
    m_instanceBufferDirty = true;
}

void MeshManager::queryVisible(const float viewMatrix[16], const float projMatrix[16],
                                uint32_t* outInstanceIds, uint32_t* outCount,
                                uint32_t maxOut) {
    float viewProj[16];
    mat4Multiply(projMatrix, viewMatrix, viewProj);

    uint32_t count = 0;
    const float margin = 0.05f;

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_instances.size()); ++i) {
        const auto& inst = m_instances[i];
        if (!(inst.flags & 1u) || inst.meshDenseIdx == UINT32_MAX) continue;

        const MeshEntry* mesh = nullptr;
        if (inst.meshDenseIdx < m_meshes.size())
            mesh = &m_meshes.dense_data()[inst.meshDenseIdx];
        if (!mesh || mesh->deleted) continue;

        float centerX = (mesh->bbox[0] + mesh->bbox[3]) * 0.5f;
        float centerY = (mesh->bbox[1] + mesh->bbox[4]) * 0.5f;
        float centerZ = (mesh->bbox[2] + mesh->bbox[5]) * 0.5f;
        float center[3] = { centerX, centerY, centerZ };

        float worldCenter[3];
        mat4TransformPoint(inst.modelMatrix, center, worldCenter);

        float clip[4];
        clip[0] = viewProj[0] * worldCenter[0] + viewProj[4] * worldCenter[1] + viewProj[8]  * worldCenter[2] + viewProj[12];
        clip[1] = viewProj[1] * worldCenter[0] + viewProj[5] * worldCenter[1] + viewProj[9]  * worldCenter[2] + viewProj[13];
        clip[2] = viewProj[2] * worldCenter[0] + viewProj[6] * worldCenter[1] + viewProj[10] * worldCenter[2] + viewProj[14];
        clip[3] = viewProj[3] * worldCenter[0] + viewProj[7] * worldCenter[1] + viewProj[11] * worldCenter[2] + viewProj[15];

        if (std::abs(clip[3]) < 1e-6f) continue;

        float ndcX = clip[0] / clip[3];
        float ndcY = clip[1] / clip[3];
        float ndcZ = clip[2] / clip[3];

        if (ndcX < -1.0f - margin || ndcX > 1.0f + margin) continue;
        if (ndcY < -1.0f - margin || ndcY > 1.0f + margin) continue;
        if (ndcZ < -1.0f - margin || ndcZ > 1.0f + margin) continue;

        if (count >= maxOut) break;
        outInstanceIds[count++] = i;
    }

    *outCount = count;
}

void MeshManager::update() {
    if (m_meshBufferDirty)
        uploadMeshBuffers(m_device);
    if (m_instanceBufferDirty)
        uploadInstanceBuffer(m_device);
}

void MeshManager::render(rhi::IDevice* device, const float viewMatrix[16],
                          const float projMatrix[16]) {
    if (m_meshBufferDirty)
        uploadMeshBuffers(device);

    uint32_t visibleIds[4096];
    uint32_t visibleCount = 0;
    queryVisible(viewMatrix, projMatrix, visibleIds, &visibleCount, 4096);

    if (visibleCount == 0) return;

    m_visibleInstances.clear();
    m_visibleInstances.reserve(visibleCount);
    for (uint32_t i = 0; i < visibleCount; ++i)
        m_visibleInstances.push_back(visibleIds[i]);

    struct VisibleInstance {
        uint32_t instanceId;
        uint32_t meshDenseIdx;
    };
    std::vector<VisibleInstance> sorted;
    sorted.reserve(visibleCount);
    for (uint32_t i = 0; i < visibleCount; ++i) {
        uint32_t instId = visibleIds[i];
        sorted.push_back({instId, m_instances[instId].meshDenseIdx});
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const VisibleInstance& a, const VisibleInstance& b) {
                  return a.meshDenseIdx < b.meshDenseIdx;
              });

    std::vector<InstanceDesc> packedInstances;
    packedInstances.reserve(visibleCount);
    for (uint32_t i = 0; i < visibleCount; ++i) {
        uint32_t instId = sorted[i].instanceId;
        const auto& inst = m_instances[instId];
        InstanceDesc d;
        std::memcpy(d.modelMatrix, inst.modelMatrix, 64);
        d.meshIndex     = inst.meshDenseIdx;
        d.materialIndex = inst.materialIndex;
        d.flags         = inst.flags;
        d._pad          = 0;
        packedInstances.push_back(d);
    }

    if (m_instanceBuffer != rhi::NullHandle)
        device->destroyBuffer(m_instanceBuffer);
    m_instanceBuffer = rhi::NullHandle;

    if (!packedInstances.empty()) {
        rhi::BufferDesc desc;
        desc.size       = packedInstances.size() * sizeof(InstanceDesc);
        desc.usage      = rhi::BufferUsage::ShaderVisible;
        desc.memory     = rhi::MemoryType::GPU_Only;
        desc.debugName  = "MeshManager_InstanceBuffer";
        m_instanceBuffer = device->createBuffer(desc);
        device->uploadBuffer(m_instanceBuffer, 0,
                             packedInstances.size() * sizeof(InstanceDesc),
                             packedInstances.data());
    }

    device->bindPipeline(m_meshPipeline);
    device->bindVertexBuffer(0, m_positionBuffer, 0);
    device->bindIndexBuffer(m_indexBuffer, 0);

    if (m_instanceBuffer != rhi::NullHandle) {
        device->bindUniformBuffer(0, 0, m_instanceBuffer, 0,
                                  packedInstances.size() * sizeof(InstanceDesc));
    }

    uint32_t drawInstanceOffset = 0;
    uint32_t groupStart = 0;
    while (groupStart < visibleCount) {
        uint32_t currentMesh = sorted[groupStart].meshDenseIdx;
        uint32_t groupEnd = groupStart;
        while (groupEnd < visibleCount && sorted[groupEnd].meshDenseIdx == currentMesh)
            ++groupEnd;

        uint32_t instanceCount = groupEnd - groupStart;

        const MeshEntry* mesh = nullptr;
        if (currentMesh < m_meshes.size())
            mesh = &m_meshes.dense_data()[currentMesh];

        if (mesh && !mesh->deleted) {
            device->drawIndexed(mesh->indexCount,
                                instanceCount,
                                mesh->indexOffset,
                                static_cast<int32_t>(mesh->vertexOffset),
                                drawInstanceOffset);
        }

        drawInstanceOffset += instanceCount;
        groupStart = groupEnd;
    }

    m_instanceBufferDirty = false;
}

} // namespace core
} // namespace render
