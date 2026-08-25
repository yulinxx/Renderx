#include "rendererRuntime.h"
#include "Log/SyLogger.h"

#include <algorithm>

namespace Render
{
    namespace RT
    {

        namespace
        {
            static const char* kWorldVert = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
uniform mat4 uView;
out vec3 vColor;
void main(){ vColor = aColor; gl_Position = uView * vec4(aPos, 1.0); })";

            static const char* kWorldFrag = R"(#version 330 core
in vec3 vColor;
out vec4 frag;
void main(){ frag = vec4(vColor, 1.0); })";

            static const char* kWorldPointFrag = R"(#version 330 core
in vec3 vColor;
out vec4 frag;
uniform float uPointSize;
void main(){
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    if (dot(d, d) > 1.0) discard;
    frag = vec4(vColor, 1.0);
})";

            static const char* kScreenVert = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
uniform vec2 uViewport;
out vec3 vColor;
void main(){
    vec2 c = vec2(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0);
    vColor = aColor;
    gl_Position = vec4(c, 0.0, 1.0);
})";

            static const char* kScreenPointVert = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
uniform vec2 uViewport;
uniform float uPointSize;
out vec3 vColor;
void main(){
    vec2 c = vec2(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0);
    vColor = aColor;
    gl_Position = vec4(c, 0.0, 1.0);
    gl_PointSize = uPointSize;
})";

            static const char* kScreenTexVert = R"(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform vec2 uViewport;
out vec2 vUV;
out vec4 vColor;
void main(){
    vec2 c = vec2(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0);
    vUV = aUV; vColor = aColor;
    gl_Position = vec4(c, 0.0, 1.0);
})";

            static const char* kScreenTexFrag = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 frag;
void main(){ frag = texture(uTex, vUV) * vColor; })";

            // ---- P3C4 (alpha-aware) variants ----
            static const char* kWorldVert4 = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aColor;
uniform mat4 uView;
out vec4 vColor;
void main(){ vColor = aColor; gl_Position = uView * vec4(aPos, 1.0); })";

            static const char* kWorldFrag4 = R"(#version 330 core
in vec4 vColor;
out vec4 frag;
void main(){ frag = vec4(vColor.rgb, vColor.a); })";

            static const char* kScreenVert4 = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aColor;
uniform vec2 uViewport;
out vec4 vColor;
void main(){
    vec2 c = vec2(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0);
    vColor = aColor;
    gl_Position = vec4(c, 0.0, 1.0);
})";

            static const char* kScreenFrag4 = R"(#version 330 core
in vec4 vColor;
out vec4 frag;
void main(){ frag = vec4(vColor.rgb, vColor.a); })";

            static RHI::PrimitiveTopology mapTopology(RTPrimitiveTopology t)
            {
                return static_cast<RHI::PrimitiveTopology>(static_cast<uint8_t>(t));
            }
            static RHI::VertexFormat mapVertexFormat(RTVertexFormat f)
            {
                return static_cast<RHI::VertexFormat>(static_cast<uint8_t>(f));
            }
            static RHI::BlendFactor mapBlend(RTBlendFactor b)
            {
                switch (b)
                {
                case RTBlendFactor::One: return RHI::BlendFactor::One;
                case RTBlendFactor::SrcAlpha: return RHI::BlendFactor::SrcAlpha;
                case RTBlendFactor::OneMinusSrcAlpha: return RHI::BlendFactor::OneMinusSrcAlpha;
                default: return RHI::BlendFactor::Zero;
                }
            }
            static RHI::CompareFunc mapDepth(RTDepthFunc d)
            {
                switch (d)
                {
                case RTDepthFunc::Less: return RHI::CompareFunc::Less;
                case RTDepthFunc::LessEqual: return RHI::CompareFunc::LessEqual;
                case RTDepthFunc::Greater: return RHI::CompareFunc::Greater;
                default: return RHI::CompareFunc::Always;
                }
            }
        }

        Runtime::Runtime() = default;
        Runtime::~Runtime() { destroy(); }

        bool Runtime::create(const RuntimeDesc* desc)
        {
            if (!desc)
                return false;

            if (desc->existingDevice)
            {
                // 共享模式：复用外部已有的 RHI 设备，Runtime 不创建/不拥有，
                // 帧生命周期（beginFrame/clear/present）由外部（旧 renderFrame）负责。
                m_device = desc->existingDevice;
                m_ownedDevice = false;
            }
            else
            {
                // 后端不可用时返回失败，不做静默回退。
                // 旧实现在 Vulkan/Metal 未编译时悄悄换成 Null 后端，
                // 结果是画面全黑而调用方拿不到任何错误信号；default 分支
                // 还会把任意非法枚举值当作 OpenGL 处理。
                switch (desc->backend)
                {
                case Backend::OpenGL: m_device = RHI::createGLDevice(); break;
                case Backend::Null: m_device = RHI::createNullDevice(); break;
                case Backend::Vulkan:
                    SY_ERRORF("Runtime: Vulkan backend not available in this build");
                    return false;
                case Backend::Metal:
                    SY_ERRORF("Runtime: Metal backend not available in this build");
                    return false;
                default:
                    SY_ERRORF("Runtime: unknown backend value %d",
                              static_cast<int>(desc->backend));
                    return false;
                }

                if (!m_device)
                    return false;

                if (!m_device->initialize(desc->nativeWindowHandle, desc->width, desc->height))
                {
                    m_device->shutdown();
                    delete m_device;
                    m_device = nullptr;
                    return false;
                }

                m_ownedDevice = true;
            }
            if (!m_psm.initialize(m_device))
            {
                SY_WARNF("Runtime: PSM init failed");
            }

            m_transientCapacity = desc->transientBufferSize > 0 ? desc->transientBufferSize : 64 * 1024 * 1024;
            RHI::BufferDesc tb{};
            tb.size = m_transientCapacity;
            tb.usage = RHI::BufferUsage::Vertex;
            tb.memory = RHI::MemoryType::GPU_CPU_Coherent;
            tb.debugName = "RT_Transient";
            m_transientBuffer = m_device->createBuffer(tb);
            if (m_transientBuffer == RHI::NullHandle)
            {
                SY_WARNF("Runtime: transient buffer creation failed");
            }
            else
            {
                m_transientId = m_nextBufferId++;
                m_buffers[m_transientId] = m_transientBuffer;
            }

            // Slot 0 is reserved as the "invalid" material index.
            m_materials.clear();
            m_materials.resize(1);

            ensureDefaultPipelines();
            return true;
        }

        void Runtime::destroy()
        {
            if (m_transientActive)
                frameEnd();

            // Runtime 自己创建的资源始终由 Runtime 销毁（无论设备是否自有）。
            if (m_device)
            {
                for (auto& kv : m_buffers)
                    if (kv.second != RHI::NullHandle)
                        m_device->destroyBuffer(kv.second);
                for (auto& p : m_pipelines)
                    if (p != RHI::NullHandle)
                        m_device->destroyPipeline(p);
                for (auto& kv : m_textures)
                    if (kv.second != RHI::NullHandle)
                        m_device->destroyTexture(kv.second);
                m_psm.shutdown();
            }
            m_buffers.clear();
            m_pipelines.clear();
            m_textures.clear();

            // 关键修复：设备只在自有时才 shutdown + delete。
            // 旧实现无条件 delete m_device，完全忽略 m_ownedDevice——
            // 而共享模式（RuntimeDesc::existingDevice）恰恰是宿主的主路径：
            // 宿主的 RenderDevice 仍持有同一个 IDevice，runtimeDestroy 之后
            // 宿主再销毁自己的设备，导致同一对象被 shutdown 两次、delete 两次。
            if (m_device && m_ownedDevice)
            {
                m_device->shutdown();
                delete m_device;
            }
            m_device = nullptr;
            m_ownedDevice = false;

            m_transientBuffer = RHI::NullHandle;
            m_transientId = 0;
            m_transientCursor = 0;
            m_materials.clear();
            m_nextBufferId = 1;
            m_nextTextureId = 1;
            m_defaultsReady = false;
        }

        void Runtime::ensureDefaultPipelines()
        {
            if (m_defaultsReady)
                return;

            auto make = [&](RTPrimitiveTopology topo, RTVertexFormat fmt, const char* vs, const char* fs,
                           bool depth, bool blend, bool pointVert) {
                RTPipelineDesc d{};
                d.topology = topo;
                d.vertexFormat = fmt;
                d.depthTest = depth ? 1 : 0;
                d.depthWrite = depth ? 1 : 0;
                d.blendEnable = blend ? 1 : 0;
                d.vertexShader = pointVert ? kScreenPointVert : vs;
                d.fragmentShader = fs;
                PipelineHandle h = createPipeline(&d);
                return static_cast<uint16_t>(h);
            };

            m_defaults[static_cast<int>(DefaultPipeline::WorldLine)] =
                make(RTPrimitiveTopology::LineStrip, RTVertexFormat::P3C3, kWorldVert, kWorldFrag, false, true, false);
            m_defaults[static_cast<int>(DefaultPipeline::WorldTri)] =
                make(RTPrimitiveTopology::Triangles, RTVertexFormat::P3C3, kWorldVert, kWorldFrag, false, true, false);
            m_defaults[static_cast<int>(DefaultPipeline::WorldPoint)] =
                make(RTPrimitiveTopology::Points, RTVertexFormat::P3C3, kWorldVert, kWorldPointFrag, false, true, false);
            m_defaults[static_cast<int>(DefaultPipeline::ScreenLine)] =
                make(RTPrimitiveTopology::LineStrip, RTVertexFormat::P3C3, kScreenVert, kWorldFrag, false, true, false);
            m_defaults[static_cast<int>(DefaultPipeline::ScreenTri)] =
                make(RTPrimitiveTopology::Triangles, RTVertexFormat::P3C3, kScreenVert, kWorldFrag, false, true, false);
            m_defaults[static_cast<int>(DefaultPipeline::ScreenPoint)] =
                make(RTPrimitiveTopology::Points, RTVertexFormat::P3C3, kScreenPointVert, kWorldPointFrag, false, true, true);
            m_defaults[static_cast<int>(DefaultPipeline::ScreenTextured)] =
                make(RTPrimitiveTopology::Triangles, RTVertexFormat::P2T2C4, kScreenTexVert, kScreenTexFrag, false, true, false);

            // P3C4 (alpha-aware) variants
            m_defaults[static_cast<int>(DefaultPipeline::WorldLine4)] =
                make(RTPrimitiveTopology::LineStrip, RTVertexFormat::P3C4, kWorldVert4, kWorldFrag4, false, true, false);
            m_defaults[static_cast<int>(DefaultPipeline::WorldTri4)] =
                make(RTPrimitiveTopology::Triangles, RTVertexFormat::P3C4, kWorldVert4, kWorldFrag4, false, true, false);
            m_defaults[static_cast<int>(DefaultPipeline::WorldPoint4)] =
                make(RTPrimitiveTopology::Points, RTVertexFormat::P3C4, kWorldVert4, kWorldFrag4, false, true, false);
            m_defaults[static_cast<int>(DefaultPipeline::ScreenLine4)] =
                make(RTPrimitiveTopology::LineStrip, RTVertexFormat::P3C4, kScreenVert4, kScreenFrag4, false, true, false);
            m_defaults[static_cast<int>(DefaultPipeline::ScreenTri4)] =
                make(RTPrimitiveTopology::Triangles, RTVertexFormat::P3C4, kScreenVert4, kScreenFrag4, false, true, false);
            m_defaults[static_cast<int>(DefaultPipeline::ScreenPoint4)] =
                make(RTPrimitiveTopology::Points, RTVertexFormat::P3C4, kScreenVert4, kScreenFrag4, false, true, false);

            m_defaultsReady = true;
        }

        BufferHandle Runtime::createBuffer(const RTBufferDesc* desc)
        {
            if (!desc || !m_device)
                return 0;
            RHI::BufferDesc bd{};
            bd.size = desc->size;
            bd.usage = static_cast<RHI::BufferUsage>(desc->usageFlags);
            bd.memory = static_cast<RHI::MemoryType>(desc->memoryFlags);
            bd.debugName = "RT_Buffer";
            RHI::BufferHandle h = m_device->createBuffer(bd);
            if (h == RHI::NullHandle)
                return 0;
            uint64_t id = m_nextBufferId++;
            m_buffers[id] = h;
            return id;
        }

        void Runtime::destroyBuffer(BufferHandle buffer)
        {
            auto it = m_buffers.find(buffer);
            if (it != m_buffers.end())
            {
                if (it->second != RHI::NullHandle)
                    m_device->destroyBuffer(it->second);
                m_buffers.erase(it);
            }
        }

        void Runtime::uploadBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, const void* data)
        {
            auto it = m_buffers.find(buffer);
            if (it != m_buffers.end() && it->second != RHI::NullHandle)
                m_device->uploadBuffer(it->second, offset, size, data);
        }

        RHI::BufferHandle Runtime::rhiBuffer(BufferHandle buffer) const
        {
            auto it = m_buffers.find(buffer);
            if (it != m_buffers.end())
                return it->second;
            return RHI::NullHandle;
        }

        PipelineHandle Runtime::createPipeline(const RTPipelineDesc* desc)
        {
            if (!desc || !m_device)
                return 0;
            RHI::PipelineDesc pd{};
            pd.topology = mapTopology(desc->topology);
            pd.vertexShader = desc->vertexShader;
            pd.fragmentShader = desc->fragmentShader;
            pd.vertexFormat = mapVertexFormat(desc->vertexFormat);
            pd.depthTest = desc->depthTest != 0;
            pd.depthWrite = desc->depthWrite != 0;
            pd.blendEnable = desc->blendEnable != 0;
            pd.srcBlend = mapBlend(desc->srcBlend);
            pd.dstBlend = mapBlend(desc->dstBlend);
            pd.depthFunc = mapDepth(desc->depthFunc);
            RHI::PipelineHandle h = m_psm.getOrCreatePipeline(pd);
            if (h == RHI::NullHandle)
                return 0;
            m_pipelines.push_back(h);
            return static_cast<PipelineHandle>(m_pipelines.size());
        }

        RHI::PipelineHandle Runtime::rhiPipeline(PipelineHandle handle) const
        {
            uint64_t idx = static_cast<uint64_t>(handle);
            if (idx >= 1 && idx <= m_pipelines.size())
                return m_pipelines[idx - 1];
            return RHI::NullHandle;
        }

        PipelineHandle Runtime::resolvePipeline(const RTDrawCommand& cmd)
        {
            RTVertexFormat fmt = static_cast<RTVertexFormat>(cmd.vertexFormat);
            RTPrimitiveTopology topo = cmd.topology;
            bool isPoint = (topo == RTPrimitiveTopology::Points);
            bool isScreen = (cmd.space == RenderSpace::Screen);

            const char* vs = nullptr;
            const char* fs = nullptr;
            if (fmt == RTVertexFormat::P3C4)
            {
                // 统一使用 *Vert4（vec4 顶点着色器）。kScreenPointVert4 在部分驱动上与
                // kScreenFrag4 链接失败（vColor 类型不匹配的误报），故圆点退化为方块点。
                vs = isScreen ? kScreenVert4 : kWorldVert4;
                fs = isScreen ? kScreenFrag4 : kWorldFrag4;
            }
            else if (fmt == RTVertexFormat::P2T2C4)
            {
                vs = kScreenTexVert;
                fs = kScreenTexFrag;
            }
            else  // P3C3
            {
                vs = isScreen ? (isPoint ? kScreenPointVert : kScreenVert) : (isPoint ? kWorldVert : kWorldVert);
                fs = isPoint ? kWorldPointFrag : kWorldFrag;
            }

            RTPipelineDesc d{};
            d.topology = topo;
            d.vertexFormat = fmt;
            d.depthTest = 0;
            d.depthWrite = 0;
            d.blendEnable = 1;
            d.srcBlend = RTBlendFactor::SrcAlpha;
            d.dstBlend = RTBlendFactor::OneMinusSrcAlpha;
            d.vertexShader = vs;
            d.fragmentShader = fs;
            return createPipeline(&d);
        }

        uint16_t Runtime::defaultPipeline(DefaultPipeline kind)
        {
            if (!m_defaultsReady)
                ensureDefaultPipelines();
            int i = static_cast<int>(kind);
            if (i < 0 || i >= static_cast<int>(DefaultPipeline::Count))
                return 0;
            return m_defaults[i];
        }

        TextureHandle Runtime::createTexture(const RTTextureDesc* desc)
        {
            if (!desc || !m_device || desc->width == 0 || desc->height == 0)
                return 0;
            RHI::TextureDesc td{};
            td.width = desc->width;
            td.height = desc->height;
            td.format = RHI::Format::RGBA8;
            td.mipLevels = 1;
            td.debugName = "RT_Texture";
            RHI::TextureHandle h = m_device->createTexture(td);
            if (h == RHI::NullHandle)
                return 0;
            if (desc->rgba && desc->rgbaBytes >= desc->width * desc->height * 4)
                m_device->uploadTexture(h, 0, desc->rgba, desc->width * 4);
            uint64_t id = m_nextTextureId++;
            m_textures[id] = h;
            return id;
        }

        void Runtime::destroyTexture(TextureHandle texture)
        {
            auto it = m_textures.find(texture);
            if (it != m_textures.end())
            {
                if (it->second != RHI::NullHandle)
                    m_device->destroyTexture(it->second);
                m_textures.erase(it);
            }
        }

        void Runtime::updateTexture(TextureHandle texture, const RTTextureDesc* desc)
        {
            auto it = m_textures.find(texture);
            if (it != m_textures.end() && it->second != RHI::NullHandle && desc->rgba)
                m_device->uploadTexture(it->second, 0, desc->rgba, desc->width * 4);
        }

        RHI::TextureHandle Runtime::rhiTexture(TextureHandle texture) const
        {
            auto it = m_textures.find(texture);
            if (it != m_textures.end())
                return it->second;
            return RHI::NullHandle;
        }

        uint16_t Runtime::addMaterial(const MaterialDesc* desc)
        {
            uint16_t idx = static_cast<uint16_t>(m_materials.size());
            m_materials.push_back(*desc);
            return idx;
        }

        void Runtime::updateMaterial(uint16_t index, const MaterialDesc* desc)
        {
            if (index < m_materials.size())
                m_materials[index] = *desc;
        }

        const MaterialDesc* Runtime::material(uint16_t index) const
        {
            if (index < m_materials.size())
                return &m_materials[index];
            return nullptr;
        }

        void Runtime::frameBegin()
        {
            if (m_transientActive || m_transientBuffer == RHI::NullHandle)
                return;
            // 不映射 GPU 缓冲：使用 CPU 暂存缓冲写入，绘制前通过 uploadBuffer 推送到 GPU。
            // 这样兼容不支持 ARB_buffer_storage（GL 持久映射）的平台（如 macOS GL 4.1）。
            if (m_transientStaging.size() != static_cast<size_t>(m_transientCapacity))
                m_transientStaging.resize(static_cast<size_t>(m_transientCapacity));
            m_transientCursor = 0;
            m_transientActive = true;
        }

        RTTransientAlloc Runtime::allocTransient(uint64_t size)
        {
            RTTransientAlloc a{};
            if (!m_transientActive || m_transientBuffer == RHI::NullHandle)
                return a;
            uint64_t aligned = (size + 15) & ~static_cast<uint64_t>(15);
            if (m_transientCursor + aligned > m_transientCapacity)
            {
                SY_WARNF("Runtime: transient buffer exhausted (cursor=%llu, need=%llu, cap=%llu)",
                    (unsigned long long)m_transientCursor, (unsigned long long)aligned,
                    (unsigned long long)m_transientCapacity);
                return a;
            }
            a.handle = m_transientId;
            a.offset = static_cast<uint32_t>(m_transientCursor);
            a.size = static_cast<uint32_t>(size);
            a.cpuPtr = m_transientStaging.data() + m_transientCursor;
            m_transientCursor += aligned;
            return a;
        }

        void Runtime::frameEnd()
        {
            if (!m_transientActive)
                return;
            m_transientCursor = 0;
            m_transientActive = false;
        }

        uint64_t Runtime::gpuMemoryBytes() const
        {
            return m_device ? m_device->getGPUMemoryUsage() : 0;
        }

    }  // namespace RT
}  // namespace Render
