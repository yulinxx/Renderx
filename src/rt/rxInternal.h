/**
 * @file rxInternal.h
 * @brief RT 层内部结构：Runtime / Surface / Session
 *
 * 这一层把公共 ABI（include/render/renderx.h，全 POD、零 STL）翻译成
 * 内部 RHI 调用。翻译边界只在本目录内，公共头里看不到任何 RHI 类型。
 *
 * 与被替换掉的 rendererRuntime/rendererSession 的结构性差异：
 *
 * 1. **一个 Runtime = 一个设备 + N 个 Surface**。旧 Runtime 的
 *    RuntimeDesc 里带 nativeWindowHandle/width/height，设备与窗口一对一
 *    绑死；第二个窗口只能再建一个 Runtime，于是各自持有一份 2048x2048
 *    字体图集与全套管线。窗口参数现在下移到 SurfaceDesc。
 * 2. **句柄是世代式的**（core/slotMap）。旧实现用 `unordered_map<uint64,...>`
 *    + 单调递增 id，句柄无校验、非法值直接解引用；Runtime/Session 句柄
 *    更是裸 `reinterpret_cast` 指针。
 * 3. **管线不再每帧 push_back**。旧 resolvePipeline 每帧每笔命令都往
 *    m_pipelines 追加一个重复句柄，且 destroy 时与 PipelineStateManager
 *    重复销毁同一批句柄（双重释放）。这里按 (格式, 空间, 拓扑, 状态)
 *    做键缓存，只建一次。
 * 4. **pushConstants 取代字符串 uniform**。所有 RT shader 共用一个
 *    std140 块 PushConstants（见 kPushConstantBytes），Session 每帧写一次。
 */
#pragma once

#include "render/renderx.h"

#include "core/slotMap.h"
#include "rhi/rhiCommandList.h"
#include "rhi/rhiGpuDevice.h"
#include "rhi/rhiLog.h"
#include "rhi/rhiSurface.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Render::RT::detail
{

    /**
     * @brief 所有 RT shader 共用的 pushConstant 块（std140 布局）
     *
     * 与 shader 里的 `layout(std140) uniform PushConstants` 逐字段对应。
     * 任何一侧改动都必须同步另一侧——std140 不会报错，只会算出错误的偏移。
     *
     * std140 偏移：uView 0..63（mat4，16 对齐）、uViewport 64..71（vec2，8 对齐）、
     * uPointSize 72..75、uSdfScale 76..79。共 80 字节，在 RHI 的 128 上限内。
     */
    struct PushConstants
    {
        float view[16]{};      ///< 列主序；Screen 空间管线忽略此值
        float viewport[2]{};   ///< 视口像素尺寸，屏幕空间与 WorldPinned 需要
        float pointSize = 1.0f;
        float sdfScale = 4.0f;
    };
    static_assert(sizeof(PushConstants) == 80, "PushConstants 与 shader 中的 std140 块必须一致");

    constexpr uint32_t kPushConstantBytes = sizeof(PushConstants);

    /// 顶点格式对应的属性布局，供建管线时展开
    struct VertexLayout
    {
        RHI::VertexAttribute attributes[3]{};
        uint32_t attributeCount = 0;
        uint32_t stride = 0;
    };

    /// 某顶点格式 + 空间对应的内建 shader 名
    struct ShaderPair
    {
        const char* vertex = nullptr;
        const char* fragment = nullptr;
    };

    struct Runtime;
    struct Surface;
    struct Session;

    /// 公共日志回调 → RHI 日志回调的桥。
    /// 两侧的 LogLevel 是不同的枚举类型（公共 ABI 与内部 RHI 必须独立演进），
    /// 布局虽相同但不能用函数指针强转——那是未定义行为，且会触发
    /// -Wcast-function-type-mismatch。用一个静态桥函数显式翻译。
    struct LogBridge
    {
        rxLogCallback callback = nullptr;
        void* userData = nullptr;

        static void forward(RHI::LogLevel level, const char* message, void* userData);
    };

    // ==================== Runtime ====================

    /**
     * @brief 瞬态环形缓冲
     *
     * 每帧顶点流的分配器。实现要点：
     * - CPU 侧影子内存 + 帧末上传，不依赖持久映射，因此 Null 后端与
     *   不支持 GL_MAP_PERSISTENT_BIT 的驱动上行为一致。
     * - 双段轮转（frames-in-flight = 2）：本帧写 A 段时 GPU 可能还在读
     *   上一帧的 B 段。旧实现单段复用，靠「希望 GPU 已经读完」。
     * - 容量不足时为该次分配单独开一个缓冲，帧末释放，而不是回绕覆盖
     *   已提交的命令（回绕覆盖的表现是「画面随机少一部分」，极难定位）。
     */
    class TransientRing
    {
    public:
        /// owner 提供设备与日志出口；溢出缓冲也要在 owner 的句柄表里登记
        bool initialize(Runtime* owner, uint64_t capacityBytes);
        void shutdown();

        /// 切换到下一段并重置游标，释放上一帧的溢出缓冲
        void beginFrame();
        /// 分配 sizeBytes 字节；失败时 out->cpuPtr 为空
        bool allocate(uint64_t sizeBytes, TransientAlloc* out);
        /// 把本帧写入的区间上传到 GPU。每次 Submit 前调用。
        void flush();

        uint64_t usedBytesThisFrame() const { return m_cursor - m_segmentBase; }
        BufferHandle publicHandle() const { return m_publicHandle; }
        RHI::BufferHandle rhiBuffer() const { return m_buffer; }
        void setPublicHandle(BufferHandle handle) { m_publicHandle = handle; }

    private:
        Runtime* m_owner = nullptr;
        RHI::IGpuDevice* m_device = nullptr;
        RHI::RhiLogger m_log;
        RHI::BufferHandle m_buffer{};
        BufferHandle m_publicHandle = BufferHandle::Invalid;
        std::vector<uint8_t> m_staging;
        uint64_t m_capacity = 0;       ///< 单段容量
        uint64_t m_segmentBase = 0;    ///< 当前段起始偏移
        uint64_t m_cursor = 0;         ///< 当前写入位置（绝对偏移）
        uint64_t m_flushed = 0;        ///< 已上传到的位置
        uint32_t m_segment = 0;
        static constexpr uint32_t kSegmentCount = 2;

        /// 容量不足时的溢出缓冲，帧末统一释放
        struct Overflow
        {
            RHI::BufferHandle buffer{};
            BufferHandle publicHandle = BufferHandle::Invalid;
            std::vector<uint8_t> staging;
        };
        std::vector<Overflow> m_overflow;
    };

    /// 管线缓存键：同一组状态只建一条管线
    struct PipelineKey
    {
        VertexFormat vertexFormat = VertexFormat::P3C3;
        RenderSpace space = RenderSpace::World;
        PrimitiveTopology topology = PrimitiveTopology::Triangles;
        uint8_t depthTest = 0;
        uint8_t depthWrite = 0;
        uint8_t blendEnable = 0;
        BlendFactor srcBlend = BlendFactor::SrcAlpha;
        BlendFactor dstBlend = BlendFactor::OneMinusSrcAlpha;
        DepthFunc depthFunc = DepthFunc::LessEqual;
        /**
         * 线宽（像素）。属于管线固定状态：Vulkan 与 Metal 都不允许在命令
         * 录制期修改，因此不同线宽必须是不同管线。
         * 已按 kLineWidthQuantum 量化并按 Capabilities::maxLineWidth 钳制，
         * 否则浮点抖动会把管线数量炸开。
         */
        float lineWidth = 1.0f;
        std::string shaderName;  ///< 空表示按格式+空间取默认

        bool operator==(const PipelineKey& other) const;
    };

    struct PipelineKeyHash
    {
        size_t operator()(const PipelineKey& key) const;
    };

    struct Runtime
    {
        RHI::IGpuDevice* device = nullptr;
        /// 公共回调的落点。必须与 Runtime 同生命周期：它的地址被当作
        /// userData 传进 RHI，RHI 侧只在回调时读取。
        LogBridge logBridge;
        RHI::RhiLogger log;
        Capabilities caps{};

        std::vector<Surface*> surfaces;
        std::vector<Session*> sessions;

        /// 公共 BufferHandle → RHI 缓冲。世代式，销毁后旧句柄立即失效。
        SlotMap<uint64_t, RHI::BufferHandle> buffers;
        SlotMap<uint64_t, RHI::TextureHandle> textures;

        /// pipelineIndex（DrawCommand 里是 uint16）→ RHI 管线。
        /// 索引从 1 起算，0 恒为「无效/让 Runtime 自己解析」。
        std::vector<RHI::PipelineHandle> pipelines;
        std::unordered_map<PipelineKey, uint16_t, PipelineKeyHash> pipelineCache;
        uint16_t defaults[static_cast<size_t>(DefaultPipeline::Count)]{};

        /// materials[0] 保留为「无材质」
        std::vector<MaterialDesc> materials;

        /// shader 名 → 已创建的 RHI shader，避免同一 shader 被反复编译
        std::unordered_map<std::string, RHI::ShaderHandle> shaders;

        /// 纹理 → 绑定组。绑定组按 (纹理, 默认采样器) 懒创建并缓存。
        std::unordered_map<uint64_t, RHI::BindGroupHandle> textureBindGroups;
        RHI::SamplerHandle defaultSampler{};

        TransientRing transient;

        /**
         * 当前处于 BeginFrame/EndFrame 之间的 Session 数量。
         *
         * 瞬态环由整个 Runtime 共享，切段必须每帧只做一次。若按 Session 切，
         * 第二个窗口开帧就会把第一个窗口本帧数据所在段翻掉，
         * 表现为「某个窗口随机缺图元」——这类问题几乎无法从现象定位。
         */
        uint32_t sessionsInFrame = 0;

        /// 线宽量化步长：把连续线宽收敛到有限的管线数量
        static constexpr float kLineWidthQuantum = 0.5f;

        bool create(const RuntimeDesc& desc);
        void destroy();

        // ---- 资源 ----
        BufferHandle createBuffer(const BufferDesc& desc);
        void destroyBuffer(BufferHandle handle);
        RxResult uploadBuffer(BufferHandle handle, uint64_t offset, uint64_t sizeBytes, const void* data);
        RHI::BufferHandle resolveBuffer(BufferHandle handle);

        TextureHandle createTexture(const TextureDesc& desc);
        void destroyTexture(TextureHandle handle);
        RxResult updateTexture(TextureHandle handle, const TextureDesc& desc);
        RHI::BindGroupHandle bindGroupForTexture(TextureHandle handle);

        uint16_t addMaterial(const MaterialDesc& desc);
        RxResult updateMaterial(uint16_t index, const MaterialDesc& desc);

        // ---- 管线 ----
        uint16_t createPipeline(const PipelineDesc& desc);
        uint16_t defaultPipeline(DefaultPipeline kind) const;
        /// 按绘制命令的格式/空间/拓扑/线宽解析一条管线（带缓存）
        uint16_t resolvePipeline(VertexFormat format, RenderSpace space, PrimitiveTopology topology,
                                 float lineWidth = 1.0f);
        RHI::PipelineHandle rhiPipeline(uint16_t index);

        // ---- 表面 ----
        SurfaceHandle createSurface(const SurfaceDesc& desc);
        void destroySurface(SurfaceHandle handle);
        RxResult resizeSurface(SurfaceHandle handle, uint32_t width, uint32_t height);
        Surface* resolveSurface(SurfaceHandle handle);

    private:
        RHI::ShaderHandle shaderByName(const char* name);
        bool ensureDefaultPipelines();
        uint16_t createPipelineFromKey(const PipelineKey& key);
    };

    // ==================== Surface ====================

    struct Surface
    {
        Runtime* runtime = nullptr;
        RHI::ISurface* rhi = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        bool hasDepth = false;
        /// 是否已有 Session 绑定；多个 Session 画同一个表面会互相覆盖，
        /// 属于调用方错误，创建 Session 时会报错。
        Session* boundSession = nullptr;
    };

    // ==================== Session ====================

    struct Session
    {
        Runtime* runtime = nullptr;
        Surface* surface = nullptr;
        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float viewMatrix[16]{};
        FrameStats stats{};

        bool inFrame = false;
        RHI::ICommandList* cmd = nullptr;
        uint64_t frameId = 0;
        uint16_t drawSequence = 0;

        /// 排序用的下标数组，复用以避免每帧分配
        std::vector<uint32_t> sortScratch;

        bool create(const SessionDesc& desc);
        void destroy();

        void setClearColor(float r, float g, float b, float a);
        void setViewMatrix(const float matrix[16]);

        RxResult beginFrame();
        RxResult allocTransient(uint64_t sizeBytes, TransientAlloc* out);
        RxResult submit(const DrawPacket& packet);
        RxResult endFrame();
        RxResult queryVisibility(const float* aabbs, uint32_t aabbCount, const float viewBounds[4],
                                 VisibilityResult* out);
    };

    // ==================== 共享工具 ====================

    /// 顶点格式 → 属性布局。新增格式只需改这里与 rxVertexStride。
    VertexLayout vertexLayoutFor(VertexFormat format);
    /// 顶点格式 + 空间 + 拓扑 → 内建 shader 名
    ShaderPair defaultShadersFor(VertexFormat format, RenderSpace space, PrimitiveTopology topology);
    RHI::PrimitiveTopology toRhiTopology(PrimitiveTopology topology);
    RHI::BlendFactor toRhiBlendFactor(BlendFactor factor);
    RHI::CompareOp toRhiCompareOp(DepthFunc func);
    RHI::IndexType toRhiIndexType(IndexType type);
    RHI::BackendKind toRhiBackend(Backend backend);

    /// 句柄 ↔ 指针。Runtime/Session 句柄本质是指针，但只在本文件内转换，
    /// 且每次使用前都做非空检查——旧实现在 C API 里到处裸转，非法句柄直接崩。
    inline Runtime* asRuntime(RuntimeHandle handle)
    {
        return reinterpret_cast<Runtime*>(static_cast<uintptr_t>(handle));
    }
    inline RuntimeHandle toHandle(Runtime* runtime)
    {
        return static_cast<RuntimeHandle>(reinterpret_cast<uintptr_t>(runtime));
    }
    inline Session* asSession(SessionHandle handle)
    {
        return reinterpret_cast<Session*>(static_cast<uintptr_t>(handle));
    }
    inline SessionHandle toHandle(Session* session)
    {
        return static_cast<SessionHandle>(reinterpret_cast<uintptr_t>(session));
    }

}  // namespace Render::RT::detail
