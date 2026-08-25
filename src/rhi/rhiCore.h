/**
 * @file rhiCore.h
 * @brief RHI 后端中立的核心类型词汇表
 *
 * 设计约束（本文件是 RHI 层的唯一类型来源）：
 * 1. 不包含任何后端头文件，不使用任何后端的常量数值。
 *    旧版 BarrierFlag 直接抄了 glMemoryBarrier 的位定义，导致 Metal/Vulkan
 *    实现必须反向翻译 GL 语义；本文件用中立的 ResourceState/BarrierScope 取代。
 * 2. 不包含 include/render/ 下的公共 ABI 头。
 *    RHI 是内部实现细节，公共 ABI 是对外契约，两者必须独立演进。
 * 3. 不含 uniform 名字字符串概念。
 *    资源绑定统一走 (set, binding) 槽位模型 + PushConstant 块，
 *    这是 Vulkan/Metal 的原生模型，GL 侧用 UBO + 预解析 location 映射实现。
 * 4. 所有返回值走 RhiResult，不抛异常。RHI 位于 DLL 内部，
 *    异常不得穿越 C ABI 边界。
 */
#pragma once

#include <cstdint>

namespace Render::RHI
{

    // ==================== 结果码 ====================

    /// RHI 操作结果码。所有可能失败的操作返回此类型，不抛异常。
    enum class RhiResult : int32_t
    {
        Ok = 0,
        ErrorUnknown = -1,
        ErrorInvalidArgument = -2,
        ErrorOutOfMemory = -3,
        ErrorDeviceLost = -4,
        ErrorUnsupported = -5,       ///< 当前后端/硬件不支持该特性
        ErrorNotInitialized = -6,
        ErrorSurfaceLost = -7,       ///< 窗口表面失效，需重建
        ErrorSwapchainOutOfDate = -8,///< 尺寸变化，需重建交换链
        ErrorShaderCompilation = -9,
        ErrorResourceCreation = -10,
    };

    inline bool succeeded(RhiResult r) { return r == RhiResult::Ok; }

    // ==================== 后端标识 ====================

    enum class BackendKind : uint8_t
    {
        Null = 0,   ///< 无操作后端，用于单测与无头环境
        OpenGL = 1,
        Metal = 2,
        Vulkan = 3,
    };

    /**
     * @brief 着色器字节码语言
     *
     * 每个后端通过 Capabilities::acceptedShaderLanguage 声明自己接受的语言。
     * ShaderLibrary 依此挑选对应变体，RHI 层本身不做任何转译。
     */
    enum class ShaderLanguage : uint8_t
    {
        GlslSource = 0,  ///< GLSL 文本（OpenGL）
        SpirV = 1,       ///< SPIR-V 字（Vulkan）
        MetalLib = 2,    ///< 预编译 .metallib 二进制（Metal）
        MetalSource = 3, ///< MSL 文本（Metal 回退路径）
    };

    // ==================== 格式与资源 ====================

    enum class Format : uint8_t
    {
        Unknown = 0,
        R8Unorm,
        RG8Unorm,
        RGBA8Unorm,
        RGBA8Srgb,
        BGRA8Unorm,   ///< Metal/Vulkan 交换链常用格式
        BGRA8Srgb,
        R16Float,
        RG16Float,
        RGBA16Float,
        R32Float,
        RG32Float,
        RGBA32Float,
        R32Uint,
        D32Float,
        D24UnormS8Uint,
        D32FloatS8Uint,
    };

    /// 返回格式的每像素字节数；深度/模板格式返回其存储大小。
    constexpr uint32_t formatByteSize(Format f)
    {
        switch (f)
        {
        case Format::R8Unorm: return 1;
        case Format::RG8Unorm: return 2;
        case Format::R16Float: return 2;
        case Format::RGBA8Unorm:
        case Format::RGBA8Srgb:
        case Format::BGRA8Unorm:
        case Format::BGRA8Srgb:
        case Format::RG16Float:
        case Format::R32Float:
        case Format::R32Uint:
        case Format::D32Float:
        case Format::D24UnormS8Uint: return 4;
        case Format::RGBA16Float:
        case Format::RG32Float:
        case Format::D32FloatS8Uint: return 8;
        case Format::RGBA32Float: return 16;
        case Format::Unknown: return 0;
        }
        return 0;
    }

    constexpr bool formatIsDepth(Format f)
    {
        return f == Format::D32Float || f == Format::D24UnormS8Uint || f == Format::D32FloatS8Uint;
    }

    /// 缓冲区用途位掩码。一个缓冲区可同时具备多种用途。
    enum class BufferUsage : uint32_t
    {
        None = 0,
        Vertex = 1u << 0,
        Index = 1u << 1,
        Uniform = 1u << 2,
        Storage = 1u << 3,  ///< 计算着色器可读写（GL SSBO / VK storage / Metal buffer）
        Indirect = 1u << 4,
        TransferSrc = 1u << 5,
        TransferDst = 1u << 6,
    };

    inline BufferUsage operator|(BufferUsage a, BufferUsage b)
    {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline bool hasFlag(BufferUsage set, BufferUsage flag)
    {
        return (static_cast<uint32_t>(set) & static_cast<uint32_t>(flag)) != 0;
    }

    enum class TextureUsage : uint32_t
    {
        None = 0,
        Sampled = 1u << 0,
        ColorAttachment = 1u << 1,
        DepthStencilAttachment = 1u << 2,
        Storage = 1u << 3,
        TransferSrc = 1u << 4,
        TransferDst = 1u << 5,
    };

    inline TextureUsage operator|(TextureUsage a, TextureUsage b)
    {
        return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline bool hasFlag(TextureUsage set, TextureUsage flag)
    {
        return (static_cast<uint32_t>(set) & static_cast<uint32_t>(flag)) != 0;
    }

    /**
     * @brief 内存访问模式
     *
     * 语义按「CPU 能否直接写」和「是否需要显式 flush」区分，
     * 不暴露任何后端的内存堆概念。
     */
    enum class MemoryAccess : uint8_t
    {
        GpuOnly = 0,      ///< CPU 不可映射，需通过 TransferDst 上传
        CpuToGpu = 1,     ///< CPU 可持久映射写入，GPU 读（顶点流、UBO 环形缓冲）
        GpuToCpu = 2,     ///< GPU 写，CPU 读回（readback）
        CpuToGpuCoherent = 3,  ///< 同 CpuToGpu 但无需显式 flush
    };

    // ==================== 管线状态 ====================

    enum class PrimitiveTopology : uint8_t
    {
        PointList = 0,
        LineList,
        LineStrip,
        TriangleList,
        TriangleStrip,
    };

    enum class IndexType : uint8_t
    {
        Uint16 = 0,
        Uint32 = 1,
    };

    enum class CompareOp : uint8_t
    {
        Never = 0,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
    };

    enum class BlendFactor : uint8_t
    {
        Zero = 0,
        One,
        SrcColor,
        OneMinusSrcColor,
        SrcAlpha,
        OneMinusSrcAlpha,
        DstAlpha,
        OneMinusDstAlpha,
    };

    enum class BlendOp : uint8_t
    {
        Add = 0,
        Subtract,
        ReverseSubtract,
        Min,
        Max,
    };

    enum class CullMode : uint8_t
    {
        None = 0,
        Front,
        Back,
    };

    enum class FrontFace : uint8_t
    {
        CounterClockwise = 0,
        Clockwise,
    };

    enum class FillMode : uint8_t
    {
        Solid = 0,
        Wireframe,  ///< Metal 无原生等价，由后端在 Capabilities 中声明支持性
    };

    enum class FilterMode : uint8_t
    {
        Nearest = 0,
        Linear,
    };

    enum class AddressMode : uint8_t
    {
        Repeat = 0,
        MirrorRepeat,
        ClampToEdge,
        ClampToBorder,
    };

    // ==================== 顶点布局 ====================

    /**
     * @brief 顶点属性数据类型
     *
     * 旧版用 VertexFormat 枚举把整个顶点布局写死成 P3C3/P3C4/... 六种，
     * 每加一种布局都要改 RHI 与全部后端。改为通用属性描述，
     * 布局由调用方声明，RHI 只做转译。
     */
    enum class VertexAttribType : uint8_t
    {
        Float1 = 0,
        Float2,
        Float3,
        Float4,
        Uint8x4Norm,  ///< 归一化字节，常用于压缩颜色
        Uint32x1,
    };

    constexpr uint32_t vertexAttribByteSize(VertexAttribType t)
    {
        switch (t)
        {
        case VertexAttribType::Float1: return 4;
        case VertexAttribType::Float2: return 8;
        case VertexAttribType::Float3: return 12;
        case VertexAttribType::Float4: return 16;
        case VertexAttribType::Uint8x4Norm: return 4;
        case VertexAttribType::Uint32x1: return 4;
        }
        return 0;
    }

    /// 单个顶点属性
    struct VertexAttribute
    {
        uint32_t location = 0;                                 ///< 对应 shader 中 layout(location=N)
        uint32_t bufferSlot = 0;                               ///< 来自哪个顶点缓冲槽
        uint32_t offset = 0;                                   ///< 在该槽 stride 内的字节偏移
        VertexAttribType type = VertexAttribType::Float3;
    };

    /// 一个顶点缓冲槽的步长与推进方式
    struct VertexBufferLayout
    {
        uint32_t slot = 0;
        uint32_t stride = 0;
        bool perInstance = false;  ///< true 时按实例推进而非按顶点
    };

    static constexpr uint32_t kMaxVertexAttributes = 16;
    static constexpr uint32_t kMaxVertexBufferSlots = 4;
    static constexpr uint32_t kMaxColorAttachments = 4;
    static constexpr uint32_t kMaxDescriptorSets = 4;
    static constexpr uint32_t kMaxBindingsPerSet = 16;
    /// PushConstant 块上限。128 字节是 Vulkan 规范保证的最小值，
    /// 也够放两个 mat4；超出者应改用 UBO。
    static constexpr uint32_t kMaxPushConstantBytes = 128;

    // ==================== 资源绑定模型 ====================

    /**
     * @brief 绑定槽的资源类型
     *
     * 采用 (set, binding) 二级槽位模型：Vulkan 原生对应 descriptor set，
     * Metal 对应 argument buffer / buffer-texture index，
     * GL 后端在 createPipeline 时把 (set,binding) 预解析成 uniform block index
     * 与 texture unit，运行期不再做字符串查找。
     */
    enum class BindingType : uint8_t
    {
        UniformBuffer = 0,
        StorageBuffer,
        SampledTexture,
        StorageTexture,
    };

    struct BindingSlot
    {
        uint32_t set = 0;
        uint32_t binding = 0;
        BindingType type = BindingType::UniformBuffer;
        /// GL 后端需要名字来定位 uniform block / sampler；
        /// Vulkan/Metal 忽略此字段。仅在 createPipeline 时使用一次，
        /// 不参与运行期绘制路径。
        const char* glName = nullptr;
    };

    // ==================== 同步 ====================

    /**
     * @brief 资源访问阶段，用于表达屏障的可见性范围
     *
     * 后端中立：GL 映射为 glMemoryBarrier 位，
     * Vulkan 映射为 VkPipelineStageFlags + VkAccessFlags，
     * Metal 映射为 MTLBarrierScope / useResource。
     */
    enum class BarrierScope : uint32_t
    {
        None = 0,
        VertexInput = 1u << 0,
        IndexInput = 1u << 1,
        UniformRead = 1u << 2,
        TextureRead = 1u << 3,
        StorageReadWrite = 1u << 4,
        IndirectCommandRead = 1u << 5,
        RenderTargetWrite = 1u << 6,
        TransferReadWrite = 1u << 7,
        HostRead = 1u << 8,
        All = 0xFFFFFFFFu,
    };

    inline BarrierScope operator|(BarrierScope a, BarrierScope b)
    {
        return static_cast<BarrierScope>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline bool hasFlag(BarrierScope set, BarrierScope flag)
    {
        return (static_cast<uint32_t>(set) & static_cast<uint32_t>(flag)) != 0;
    }

    // ==================== 句柄 ====================

    /**
     * @brief 强类型资源句柄
     *
     * 旧版全部 using = uint64_t，BufferHandle 与 TextureHandle 可互传且编译器
     * 不报错。用 tag 模板生成互不兼容的类型，零运行期开销。
     */
    template <typename Tag>
    struct Handle
    {
        uint64_t value = 0;

        constexpr bool valid() const { return value != 0; }
        constexpr explicit operator bool() const { return value != 0; }
        friend constexpr bool operator==(Handle a, Handle b) { return a.value == b.value; }
        friend constexpr bool operator!=(Handle a, Handle b) { return a.value != b.value; }
    };

    struct BufferTag;
    struct TextureTag;
    struct SamplerTag;
    struct PipelineTag;
    struct ShaderTag;
    struct RenderPassTag;
    struct BindGroupTag;

    using BufferHandle = Handle<BufferTag>;
    using TextureHandle = Handle<TextureTag>;
    using SamplerHandle = Handle<SamplerTag>;
    using PipelineHandle = Handle<PipelineTag>;
    using ShaderHandle = Handle<ShaderTag>;
    using RenderPassHandle = Handle<RenderPassTag>;
    using BindGroupHandle = Handle<BindGroupTag>;

    // ==================== 几何辅助结构 ====================

    struct Viewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minDepth = 0.0f;
        float maxDepth = 1.0f;
    };

    struct Rect2D
    {
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct ColorRgba
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;
    };

    struct Extent2D
    {
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // ==================== 描述结构 ====================

    struct BufferDesc
    {
        uint64_t size = 0;
        BufferUsage usage = BufferUsage::None;
        MemoryAccess access = MemoryAccess::GpuOnly;
        const char* debugName = nullptr;
    };

    struct TextureDesc
    {
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        Format format = Format::RGBA8Unorm;
        TextureUsage usage = TextureUsage::Sampled;
        const char* debugName = nullptr;
    };

    struct SamplerDesc
    {
        FilterMode minFilter = FilterMode::Linear;
        FilterMode magFilter = FilterMode::Linear;
        FilterMode mipFilter = FilterMode::Linear;
        AddressMode addressU = AddressMode::ClampToEdge;
        AddressMode addressV = AddressMode::ClampToEdge;
        float maxAnisotropy = 1.0f;
        const char* debugName = nullptr;
    };

    /**
     * @brief 着色器字节码
     *
     * data/sizeBytes 的所有权归调用方，createShader 期间必须保持有效，
     * 返回后 RHI 不再引用。
     */
    struct ShaderDesc
    {
        ShaderLanguage language = ShaderLanguage::GlslSource;
        const void* data = nullptr;
        uint64_t sizeBytes = 0;
        const char* entryPoint = "main";
        const char* debugName = nullptr;
    };

    struct ColorBlendState
    {
        bool enable = false;
        BlendFactor srcColor = BlendFactor::SrcAlpha;
        BlendFactor dstColor = BlendFactor::OneMinusSrcAlpha;
        BlendOp colorOp = BlendOp::Add;
        BlendFactor srcAlpha = BlendFactor::One;
        BlendFactor dstAlpha = BlendFactor::OneMinusSrcAlpha;
        BlendOp alphaOp = BlendOp::Add;
    };

    struct DepthStencilState
    {
        bool depthTestEnable = false;
        bool depthWriteEnable = false;
        CompareOp depthCompare = CompareOp::LessEqual;
        Format format = Format::Unknown;  ///< Unknown 表示无深度附件
    };

    struct RasterState
    {
        FillMode fillMode = FillMode::Solid;
        CullMode cullMode = CullMode::None;
        FrontFace frontFace = FrontFace::CounterClockwise;
        /// 线宽。GL 之外的后端普遍不支持 >1；
        /// 见 Capabilities::maxLineWidth，超出时应改用三角化线段。
        float lineWidth = 1.0f;
    };

    /**
     * @brief 图形/计算管线描述
     *
     * 与旧版的关键差异：
     * - 顶点布局由 attributes/bufferLayouts 显式声明，不再是固定枚举
     * - shader 以句柄形式传入，编译在 createShader 阶段完成并可缓存
     * - 绑定槽显式声明，GL 后端在此一次性解析名字
     * - 显式声明颜色/深度附件格式，Metal/Vulkan 创建管线时必需
     */
    struct GraphicsPipelineDesc
    {
        ShaderHandle vertexShader{};
        ShaderHandle fragmentShader{};

        PrimitiveTopology topology = PrimitiveTopology::TriangleList;

        const VertexAttribute* attributes = nullptr;
        uint32_t attributeCount = 0;
        const VertexBufferLayout* bufferLayouts = nullptr;
        uint32_t bufferLayoutCount = 0;

        const BindingSlot* bindings = nullptr;
        uint32_t bindingCount = 0;
        uint32_t pushConstantBytes = 0;

        RasterState raster{};
        DepthStencilState depthStencil{};
        ColorBlendState blend[kMaxColorAttachments]{};
        Format colorFormats[kMaxColorAttachments]{};
        uint32_t colorAttachmentCount = 1;

        const char* debugName = nullptr;
    };

    struct ComputePipelineDesc
    {
        ShaderHandle computeShader{};
        const BindingSlot* bindings = nullptr;
        uint32_t bindingCount = 0;
        uint32_t pushConstantBytes = 0;
        const char* debugName = nullptr;
    };

    // ==================== 渲染目标 ====================

    enum class LoadOp : uint8_t
    {
        Load = 0,   ///< 保留先前内容
        Clear,      ///< 清除为 clearValue
        DontCare,   ///< 内容未定义（tile 架构上最快）
    };

    enum class StoreOp : uint8_t
    {
        Store = 0,
        DontCare,
    };

    struct ColorAttachment
    {
        TextureHandle texture{};   ///< 无效句柄表示使用交换链当前后备缓冲
        uint32_t mipLevel = 0;
        LoadOp loadOp = LoadOp::Clear;
        StoreOp storeOp = StoreOp::Store;
        ColorRgba clearValue{};
    };

    struct DepthAttachment
    {
        TextureHandle texture{};
        LoadOp loadOp = LoadOp::Clear;
        StoreOp storeOp = StoreOp::DontCare;
        float clearDepth = 1.0f;
        uint8_t clearStencil = 0;
    };

    /**
     * @brief 渲染通道开始描述
     *
     * 显式的 begin/end RenderPass 是 Vulkan/Metal 的强制模型，
     * 也让 GL 后端能一次性完成 FBO 绑定 + clear，
     * 取代旧版散落的 setClearColor/clear/bindDefaultTarget 即时状态调用。
     */
    struct RenderPassBeginDesc
    {
        ColorAttachment colorAttachments[kMaxColorAttachments]{};
        uint32_t colorAttachmentCount = 1;
        DepthAttachment depthAttachment{};
        bool hasDepthAttachment = false;
        Extent2D extent{};
        const char* debugName = nullptr;
    };

    // ==================== 绑定组 ====================

    struct BufferBinding
    {
        uint32_t set = 0;
        uint32_t binding = 0;
        BufferHandle buffer{};
        uint64_t offset = 0;
        uint64_t size = 0;  ///< 0 表示到缓冲区末尾
    };

    struct TextureBinding
    {
        uint32_t set = 0;
        uint32_t binding = 0;
        TextureHandle texture{};
        SamplerHandle sampler{};
    };

    struct BindGroupDesc
    {
        const BufferBinding* buffers = nullptr;
        uint32_t bufferCount = 0;
        const TextureBinding* textures = nullptr;
        uint32_t textureCount = 0;
        const char* debugName = nullptr;
    };

    // ==================== 间接绘制 ====================

    /// 与 VkDrawIndirectCommand / GL DrawArraysIndirectCommand 布局一致
    struct DrawIndirectCommand
    {
        uint32_t vertexCount = 0;
        uint32_t instanceCount = 0;
        uint32_t firstVertex = 0;
        uint32_t firstInstance = 0;
    };

    /// 与 VkDrawIndexedIndirectCommand / GL DrawElementsIndirectCommand 布局一致
    struct DrawIndexedIndirectCommand
    {
        uint32_t indexCount = 0;
        uint32_t instanceCount = 0;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        uint32_t firstInstance = 0;
    };

    // ==================== 能力查询 ====================

    /**
     * @brief 后端能力描述
     *
     * 旧版没有能力查询，上层只能靠「函数指针是否为空」推断，
     * 且 macOS 线宽限制之类的差异只存在于注释里。
     * 上层必须先查此结构再决定渲染策略。
     */
    struct Capabilities
    {
        BackendKind backend = BackendKind::Null;
        ShaderLanguage acceptedShaderLanguage = ShaderLanguage::GlslSource;

        char deviceName[128]{};
        char driverInfo[128]{};

        bool computeShaders = false;
        bool indirectDraw = false;
        bool multiDrawIndirect = false;
        bool storageBuffers = false;
        bool wireframeFill = false;      ///< Metal 为 false
        bool baseVertexOffset = false;
        bool persistentMapping = false;  ///< 支持长期映射的 CPU 可写缓冲
        bool timestampQueries = false;

        float maxLineWidth = 1.0f;       ///< macOS GL 与 Metal 上通常为 1.0
        uint32_t maxTextureSize = 2048;
        uint32_t maxVertexAttributes = kMaxVertexAttributes;
        uint32_t maxColorAttachments = 1;
        uint32_t maxPushConstantBytes = 0;
        uint32_t uniformBufferOffsetAlignment = 256;
        uint32_t storageBufferOffsetAlignment = 256;
        uint32_t maxFramesInFlight = 2;
    };

    // ==================== 统计 ====================

    struct FrameStats
    {
        uint32_t drawCalls = 0;
        uint32_t computeDispatches = 0;
        uint32_t pipelineSwitches = 0;
        uint32_t bindGroupSwitches = 0;
        uint64_t vertexBytesUploaded = 0;
        uint64_t gpuMemoryBytes = 0;
    };

}  // namespace Render::RHI
