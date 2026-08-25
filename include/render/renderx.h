/**
 * @file renderx.h
 * @brief RenderX 渲染 DLL 的唯一公共 ABI 头
 *
 * ==================== ABI 规则 ====================
 *
 * 本文件是 DLL 与调用方之间的完整契约，除本文件外不存在其他公共头。
 * 修改本文件即修改 ABI，必须同步 RENDERX_ABI_VERSION。
 *
 * 1. 零 STL、零 C++ 标准库类型跨界。
 *    唯一 include 是 <cstdint>。此前的 RenderTypes.h 同时扮演
 *    「DLL 公共 ABI 头」与「宿主内部 C++ 类型头」两个角色，含 7 个带
 *    std::vector 成员的结构（RenderCommand / RenderFrame /
 *    RenderOverlayUpdate 等）和返回 std::vector 的 inline 模板。
 *    虽然当时没有一个导出函数真的用到它们，但它们与 ABI 类型混在同一个头里、
 *    没有任何隔离标注——任何后续改动只要把它们放进导出签名，就会立刻引入
 *    allocator / _ITERATOR_DEBUG_LEVEL / MSVC Debug-vs-Release 不匹配的
 *    堆崩溃，而 code review 很难拦住。本文件通过物理隔离消除该风险。
 *
 * 2. 结构体全部是 POD，且全部有 static_assert 锁定大小。
 *    此前只有 6 个结构有大小断言，而最关键的入参结构（DeviceDesc）
 *    的「校验」是一个只有注释的空 #ifdef _DEBUG 块。
 *
 * 3. ABI 面不使用 bool。
 *    C++ 标准未规定 sizeof(bool)。此前 DeviceDesc::debugLayer 用 bool，
 *    renderSetSceneEnvEx 甚至在签名里直接传 const bool* 数组。
 *    本文件统一用 uint8_t（0/1）。
 *
 * 4. 句柄是 enum class : uint64_t。
 *    既保证 8 字节的 C 布局，又让 RuntimeHandle 与 SessionHandle
 *    互不兼容——此前二者都是 uint64_t 的 typedef，可以静默互传。
 *
 * 5. 数组一律「指针 + 数量」，字符串一律 const char*（UTF-8, NUL 结尾）。
 *    内存所有权单向：调用方分配的内存在调用返回后 DLL 不再引用。
 *
 * 6. 不暴露任何 DLL 内部 C++ 类型。
 *    此前 RuntimeDesc::existingDevice 是 RHI::IDevice*——一个定义在
 *    未安装的私有头 src/rhi/rhiDevice.h 中的抽象类。这把虚表布局变成了
 *    跨 DLL 契约：IDevice 任何虚函数的增删或重排都要求两侧同时重编译，
 *    否则运行期静默走错 vtable slot。该字段已移除，多窗口改由
 *    「一个 Runtime + N 个 Surface」表达（见 rxSurfaceCreate）。
 *
 * 7. 所有可失败的调用返回 RxResult，不抛异常。
 *    异常不得穿越 C ABI 边界。
 *
 * ==================== 职责边界 ====================
 *
 * 本 DLL 只回答「怎么画」：接收顶点字节流 + 绘制命令描述符，
 * 按 sortKey 排序合批后提交 GPU。
 *
 * 本 DLL 不做、也不应该做的事：
 * - 几何离散化（圆/弧/椭圆/贝塞尔 → 折线）。这是几何层职责，
 *   由应用侧的 RenderSceneBuilder 完成。此前 DLL 通过
 *   GeometryPrimitiveKind{Circle, Arc, Ellipse...} 接收解析曲线并自行
 *   细分，导致同一套离散化公式在三处重复实现，且必须靠公共头共享的
 *   inline 函数（TessParams.h）来维持一致——那是耦合，不是封装。
 * - 场景图 / 实体语义 / 图层 / 选择集 / 捕捉 / 单位制。
 * - 拾取（picking / hitTest）。
 * - 文本布局与排版（字形光栅化与图集是 GPU 资源缓存，留在 DLL 内）。
 * - 文件 IO。shader 已编入二进制，字体由调用方以内存数据注入。
 */
#ifndef RENDERX_PUBLIC_API_H
#define RENDERX_PUBLIC_API_H

#include <cstdint>

// ==================== 导出宏 ====================
//
// 注：此前该宏在 render.h 与 runtime_session.h 中被完整复制了两份。
// 现只在本文件定义一次。
#if defined(_WIN32) || defined(_WIN64)
    #ifdef RENDER_EXPORTS
        #define RENDER_API __declspec(dllexport)
    #else
        #define RENDER_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) || defined(__clang__)
    #define RENDER_API __attribute__((visibility("default")))
#else
    #define RENDER_API
#endif

// ==================== ABI 版本 ====================

#define RENDERX_ABI_VERSION_MAJOR 3
#define RENDERX_ABI_VERSION_MINOR 0
#define RENDERX_ABI_VERSION \
    ((RENDERX_ABI_VERSION_MAJOR << 16) | RENDERX_ABI_VERSION_MINOR)

namespace Render
{
    namespace RT
    {

        // ==================== 结果码 ====================

        enum class RxResult : int32_t
        {
            Ok = 0,
            ErrorUnknown = -1,
            ErrorInvalidArgument = -2,
            ErrorInvalidHandle = -3,
            ErrorOutOfMemory = -4,
            ErrorUnsupportedBackend = -5,
            ErrorDeviceLost = -6,
            ErrorSurfaceLost = -7,
            ErrorShaderCompilation = -8,
            ErrorAbiVersionMismatch = -9,
            /// 交换链尺寸失效，调用 rxSurfaceResize 后重试本帧
            ErrorSurfaceOutOfDate = -10,
        };

        // ==================== 句柄 ====================
        //
        // enum class : uint64_t —— 8 字节 C 布局 + 编译期类型隔离。
        // 0 恒为无效值。

        enum class RuntimeHandle : uint64_t { Invalid = 0 };
        enum class SurfaceHandle : uint64_t { Invalid = 0 };
        enum class SessionHandle : uint64_t { Invalid = 0 };
        enum class BufferHandle : uint64_t { Invalid = 0 };
        enum class PipelineHandle : uint64_t { Invalid = 0 };
        enum class TextureHandle : uint64_t { Invalid = 0 };

        template <typename H>
        constexpr bool rxValid(H h)
        {
            return h != H::Invalid;
        }

        // ==================== 后端 ====================

        enum class Backend : int32_t
        {
            /// 无操作后端：不产生任何 GPU 调用，用于单测与无头环境
            Null = 0,
            OpenGL = 1,
            Metal = 2,
            Vulkan = 3,
            /// 由 DLL 选择当前平台的最佳可用后端
            Auto = 4,
        };

        /// 原生窗口句柄的语义标签。
        /// 此前只有一个裸 void* nativeWindowHandle，注释写「如 HWND」——
        /// 调用方无从知道各平台该传什么，Linux 下 Vulkan 需要两个值也无处安放。
        enum class NativeWindowKind : int32_t
        {
            None = 0,
            /// handleA = HWND
            Win32Hwnd = 1,
            /// handleA = NSView*
            CocoaNsView = 2,
            /// handleA = Display*, handleB = Window
            XlibWindow = 3,
            /// handleA = wl_display*, handleB = wl_surface*
            WaylandSurface = 4,
            /// 宿主已自行创建并管理 GL 上下文（Qt QOpenGLWidget 场景）。
            /// DLL 不创建上下文，但要求宿主在 rxSessionBeginFrame 前
            /// 保证该上下文 current。
            ForeignGlContext = 5,
        };

        enum class PresentMode : int32_t
        {
            /// 不等垂直同步
            Immediate = 0,
            /// 垂直同步，各后端必须支持
            Fifo = 1,
            /// 三缓冲低延迟，不支持时自动回退 Fifo
            Mailbox = 2,
        };

        // ==================== 绘制状态 ====================

        enum class RenderSpace : uint8_t
        {
            /// 顶点经 viewMatrix 变换
            World = 0,
            /// 顶点直接是像素坐标，不随缩放变化
            Screen = 1,
        };

        enum class PrimitiveTopology : uint8_t
        {
            Points = 0,
            Lines = 1,
            LineStrip = 2,
            LineLoop = 3,
            Triangles = 4,
            TriangleStrip = 5,
        };

        enum class VertexFormat : uint8_t
        {
            /// 位置 float3 + 颜色 float3，24 字节
            P3C3 = 0,
            /// 位置 float3 + 颜色 float4（含透明度），28 字节
            P3C4 = 1,
            /// 位置 float3 + 法线 float3，24 字节
            P3N3 = 2,
            /// 位置 float2 + UV float2 + 颜色 float4，32 字节
            P2T2C4 = 3,
        };

        /// 返回某顶点格式的步长（字节）。调用方据此计算偏移，
        /// 避免在两侧各写一份硬编码常量。
        constexpr uint32_t rxVertexStride(VertexFormat fmt)
        {
            switch (fmt)
            {
            case VertexFormat::P3C3: return 24;
            case VertexFormat::P3C4: return 28;
            case VertexFormat::P3N3: return 24;
            case VertexFormat::P2T2C4: return 32;
            }
            return 0;
        }

        enum class IndexType : uint8_t
        {
            None = 0,
            Uint16 = 1,
            Uint32 = 2,
        };

        enum class BlendFactor : uint8_t
        {
            Zero = 0,
            One = 1,
            SrcAlpha = 2,
            OneMinusSrcAlpha = 3,
        };

        enum class DepthFunc : uint8_t
        {
            Always = 0,
            Less = 1,
            LessEqual = 2,
            Greater = 3,
        };

        /// 内建管线。覆盖层（选择框/手柄/虚线轮廓/点标记）统一使用
        /// 世界空间 + P3C4，以保证缩放时与图元几何一致变换。
        enum class DefaultPipeline : uint8_t
        {
            WorldLine = 0,
            WorldTri = 1,
            WorldPoint = 2,
            ScreenLine = 3,
            ScreenTri = 4,
            ScreenPoint = 5,
            ScreenTextured = 6,
            WorldLine4 = 7,
            WorldTri4 = 8,
            WorldPoint4 = 9,
            ScreenLine4 = 10,
            ScreenTri4 = 11,
            ScreenPoint4 = 12,
            Count = 13,
        };

        // ==================== 创建描述 ====================

        /// 日志级别，与 rxLogCallback 配合
        enum class LogLevel : int32_t
        {
            Debug = 0,
            Info = 1,
            Warn = 2,
            Error = 3,
        };

        /**
         * @brief 日志回调
         *
         * DLL 不再硬依赖宿主的日志库（此前 17 个源文件直接
         * #include "Log/SyLogger.h"，这是 RenderX 无法独立复用的唯一原因）。
         * message 仅在回调期间有效。
         */
        using rxLogCallback = void (*)(LogLevel level, const char* message, void* userData);

        struct RuntimeDesc
        {
            /// 必须填 RENDERX_ABI_VERSION，不匹配则 rxRuntimeCreate 失败。
            /// 这是防止「头文件与 DLL 版本不一致」导致静默错乱的第一道门。
            uint32_t abiVersion;
            Backend backend;
            /// 0/1。开启 GL debug output / Vulkan validation layer
            uint8_t enableValidation;
            uint8_t _pad0[3];
            /// 瞬态环形缓冲容量（字节）。0 表示使用默认 64MB
            uint64_t transientBufferBytes;
            rxLogCallback logCallback;
            void* logUserData;
            const char* applicationName;
        };
        static_assert(sizeof(RuntimeDesc) == 48, "RuntimeDesc ABI size changed");

        struct SurfaceDesc
        {
            NativeWindowKind windowKind;
            PresentMode presentMode;
            void* handleA;
            void* handleB;
            uint32_t width;
            uint32_t height;
            /// 0/1。是否需要深度附件
            uint8_t enableDepth;
            uint8_t _pad0[3];
            uint32_t _pad1;
        };
        static_assert(sizeof(SurfaceDesc) == 40, "SurfaceDesc ABI size changed");

        struct SessionDesc
        {
            RuntimeHandle runtime;
            /// 该 Session 渲染到哪个窗口表面
            SurfaceHandle surface;
            float clearColor[4];
        };
        static_assert(sizeof(SessionDesc) == 32, "SessionDesc ABI size changed");

        struct BufferDesc
        {
            uint64_t sizeBytes;
            /// 0/1。true 表示 CPU 需要频繁写入（顶点流、UBO），
            /// false 表示上传一次后长期只读（静态几何）
            uint8_t cpuWritable;
            uint8_t _pad0[7];
        };
        static_assert(sizeof(BufferDesc) == 16, "BufferDesc ABI size changed");

        struct PipelineDesc
        {
            PrimitiveTopology topology;
            VertexFormat vertexFormat;
            /// 0/1
            uint8_t depthTest;
            uint8_t depthWrite;
            uint8_t blendEnable;
            BlendFactor srcBlend;
            BlendFactor dstBlend;
            DepthFunc depthFunc;
            /// 内建 shader 名（见 DLL 内置 shader 库）。
            /// 空指针表示按 vertexFormat + space 使用默认 shader。
            const char* shaderName;
        };
        static_assert(sizeof(PipelineDesc) == 16, "PipelineDesc ABI size changed");

        struct TextureDesc
        {
            uint32_t width;
            uint32_t height;
            /// RGBA8 像素数据，行优先，无行填充
            const uint8_t* rgba;
            uint64_t rgbaBytes;
        };
        static_assert(sizeof(TextureDesc) == 24, "TextureDesc ABI size changed");

        struct MaterialDesc
        {
            /// 线宽（像素）。注意各后端上限不同，见 Capabilities::maxLineWidth；
            /// 超出上限时应改用三角化线段
            float lineWidth;
            float pointSize;
            float color[4];
            uint32_t flags;
        };
        static_assert(sizeof(MaterialDesc) == 28, "MaterialDesc ABI size changed");

        // ==================== 绘制命令 ====================

        /**
         * @brief 单笔绘制命令
         *
         * 纯描述符：不含任何几何语义，只有「哪段缓冲、多少顶点、
         * 什么拓扑、用哪条管线、排在哪」。
         */
        struct DrawCommand
        {
            BufferHandle vertexBuffer;
            BufferHandle indexBuffer;
            TextureHandle texture;
            /// 排序键，用 rxMakeSortKey 构造
            uint64_t sortKey;
            /// 透传给调用方的自定义数据，DLL 不解释
            uint64_t userData;
            uint32_t vertexOffset;
            uint32_t vertexCount;
            uint32_t indexOffset;
            uint32_t indexCount;
            uint32_t instanceCount;
            uint32_t firstInstance;
            PrimitiveTopology topology;
            RenderSpace space;
            VertexFormat vertexFormat;
            IndexType indexType;
            uint16_t pipelineIndex;
            uint16_t materialIndex;
            /// 0 表示沿用材质的线宽
            float lineWidth;
            float pointSize;
        };
        static_assert(sizeof(DrawCommand) == 80, "DrawCommand ABI size changed");

        /**
         * @brief 一次提交的绘制批次
         *
         * commands 指向调用方内存，rxSessionSubmit 返回后 DLL 不再引用。
         */
        struct DrawPacket
        {
            const DrawCommand* commands;
            uint32_t commandCount;
            /// 0/1。是否启用 DLL 侧的视锥裁剪
            uint8_t enableCulling;
            uint8_t _pad0[3];
            /// 列主序 4x4
            float viewMatrix[16];
            /// x, y, width, height（像素）
            float viewport[4];
            uint64_t frameId;
        };
        static_assert(sizeof(DrawPacket) == 104, "DrawPacket ABI size changed");

        /// 瞬态缓冲分配结果。cpuPtr 仅在本帧 begin/end 之间有效。
        struct TransientAlloc
        {
            BufferHandle buffer;
            void* cpuPtr;
            uint32_t offset;
            uint32_t sizeBytes;
        };
        static_assert(sizeof(TransientAlloc) == 24, "TransientAlloc ABI size changed");

        /// 可见性查询结果。indices 数组由调用方分配，DLL 只填充。
        struct VisibilityResult
        {
            uint32_t* indices;
            uint32_t count;
            uint32_t capacity;
        };
        static_assert(sizeof(VisibilityResult) == 16, "VisibilityResult ABI size changed");

        struct FrameStats
        {
            uint32_t drawCallCount;
            uint32_t triangleCount;
            uint32_t lineCount;
            uint32_t pointCount;
            uint32_t pipelineSwitches;
            uint32_t _pad0;
            uint64_t transientBytesUsed;
            uint64_t gpuMemoryBytes;
        };
        static_assert(sizeof(FrameStats) == 40, "FrameStats ABI size changed");

        /**
         * @brief 后端能力
         *
         * 调用方必须先查询再决定渲染策略。此前完全没有这一层，
         * 上层只能靠「函数指针是否为空」推断特性，而 macOS 上
         * GL 线宽只能是 1.0 这类差异只存在于注释里。
         */
        struct Capabilities
        {
            Backend backend;
            uint32_t _pad0;
            char deviceName[128];
            char driverInfo[128];
            /// 各项为 0/1
            uint8_t computeShaders;
            uint8_t indirectDraw;
            uint8_t storageBuffers;
            uint8_t wireframeFill;
            uint8_t persistentMapping;
            uint8_t timestampQueries;
            uint8_t _pad1[2];
            /// macOS GL 与 Metal 上通常为 1.0
            float maxLineWidth;
            uint32_t maxTextureSize;
            uint32_t maxColorAttachments;
            uint32_t uniformBufferOffsetAlignment;
            uint32_t maxFramesInFlight;
        };
        static_assert(sizeof(Capabilities) == 292, "Capabilities ABI size changed");

        // ==================== C API ====================
        //
        // 注：extern "C" 只去掉名字修饰，命名空间对链接符号没有任何隔离作用。
        // 此前 API 名为裸 sessionCreate / runtimeCreate，直接污染全局 C 符号
        // 空间，并会与其他库的同名符号静默冲突。全部改用 rx 前缀。
        //
        // 本头文件本身是 C++ 头（使用 enum class、模板、constexpr），
        // 不能被 C 编译器消费；符号采用 C 链接只是为了便于跨语言绑定。

        extern "C"
        {

        /// 返回 DLL 编译时的 ABI 版本，用于与 RENDERX_ABI_VERSION 比对
        RENDER_API uint32_t rxGetAbiVersion();
        RENDER_API const char* rxResultName(RxResult result);
        RENDER_API const char* rxBackendName(Backend backend);

        /// 查询某后端在当前 DLL 构建与当前机器上是否可用
        RENDER_API uint8_t rxIsBackendAvailable(Backend backend);

        // ---------- Runtime：进程内的 GPU 与共享资源 ----------

        /**
         * @brief 创建 Runtime
         *
         * 一个 Runtime 拥有一个 GPU 设备与全部共享资源（管线缓存、字体图集、
         * 缓冲池）。多窗口共享同一个 Runtime，各窗口只需各自的 Surface 与
         * Session——这是资源共享的前提。此前设备与窗口一对一绑死，
         * 每个窗口各自持有一份 2048x2048 字体图集与全套管线。
         *
         * 失败返回 Invalid，原因通过 desc->logCallback 报告。
         * 后端不可用时直接失败，不会静默回退到 Null 后端。
         */
        RENDER_API RuntimeHandle rxRuntimeCreate(const RuntimeDesc* desc);
        RENDER_API void rxRuntimeDestroy(RuntimeHandle runtime);
        RENDER_API RxResult rxRuntimeGetCapabilities(RuntimeHandle runtime, Capabilities* out);

        RENDER_API BufferHandle rxBufferCreate(RuntimeHandle runtime, const BufferDesc* desc);
        RENDER_API void rxBufferDestroy(RuntimeHandle runtime, BufferHandle buffer);
        RENDER_API RxResult rxBufferUpload(RuntimeHandle runtime, BufferHandle buffer,
                                           uint64_t offset, uint64_t sizeBytes, const void* data);

        RENDER_API PipelineHandle rxPipelineCreate(RuntimeHandle runtime, const PipelineDesc* desc);
        RENDER_API uint16_t rxPipelineGetDefault(RuntimeHandle runtime, DefaultPipeline kind);

        RENDER_API TextureHandle rxTextureCreate(RuntimeHandle runtime, const TextureDesc* desc);
        RENDER_API void rxTextureDestroy(RuntimeHandle runtime, TextureHandle texture);
        RENDER_API RxResult rxTextureUpdate(RuntimeHandle runtime, TextureHandle texture,
                                            const TextureDesc* desc);

        RENDER_API uint16_t rxMaterialAdd(RuntimeHandle runtime, const MaterialDesc* desc);
        RENDER_API RxResult rxMaterialUpdate(RuntimeHandle runtime, uint16_t index,
                                             const MaterialDesc* desc);

        /**
         * @brief 以内存数据加载字形图集所用字体
         *
         * DLL 不做文件 IO。此前 renderCreateDevice 用 std::filesystem 推导
         * 可执行文件目录并从磁盘读 default_screen_font.ttf，使 DLL 依赖运行
         * 目录布局——在 macOS .app bundle 下极易失效。
         */
        RENDER_API RxResult rxFontLoad(RuntimeHandle runtime, const void* fontData,
                                       uint64_t dataSize, float pixelHeight);

        // ---------- Surface：窗口表面 ----------

        /// 为一个窗口创建可呈现表面。同一 Runtime 可创建任意多个。
        RENDER_API SurfaceHandle rxSurfaceCreate(RuntimeHandle runtime, const SurfaceDesc* desc);
        RENDER_API void rxSurfaceDestroy(RuntimeHandle runtime, SurfaceHandle surface);
        RENDER_API RxResult rxSurfaceResize(RuntimeHandle runtime, SurfaceHandle surface,
                                            uint32_t width, uint32_t height);

        // ---------- Session：一个视口的相机与提交 ----------

        RENDER_API SessionHandle rxSessionCreate(const SessionDesc* desc);
        RENDER_API void rxSessionDestroy(SessionHandle session);
        RENDER_API void rxSessionSetClearColor(SessionHandle session, float r, float g, float b, float a);
        RENDER_API void rxSessionSetViewMatrix(SessionHandle session, const float viewMatrix[16]);

        /**
         * @brief 开始一帧
         *
         * 内部完成后备缓冲获取（GL 后端在此 makeCurrent）。
         * 返回 ErrorSurfaceOutOfDate 时调用 rxSurfaceResize 后重试。
         */
        RENDER_API RxResult rxSessionBeginFrame(SessionHandle session);

        /// 分配本帧瞬态顶点内存。仅在 BeginFrame/EndFrame 之间有效。
        RENDER_API RxResult rxSessionAllocTransient(SessionHandle session, uint64_t sizeBytes,
                                                    TransientAlloc* out);

        /// 提交一批绘制命令。同一帧内可多次调用。
        RENDER_API RxResult rxSessionSubmit(SessionHandle session, const DrawPacket* packet);

        /// 结束并呈现本帧（GL: swapBuffers, Metal: presentDrawable, VK: queuePresent）
        RENDER_API RxResult rxSessionEndFrame(SessionHandle session);

        RENDER_API RxResult rxSessionQueryVisibility(SessionHandle session, const float* aabbs,
                                                     uint32_t aabbCount, const float viewBounds[4],
                                                     VisibilityResult* out);
        RENDER_API RxResult rxSessionGetStats(SessionHandle session, FrameStats* out);

        // ---------- 工具 ----------

        /**
         * @brief 构造排序键
         *
         * 布局：layer(8) | transparent(8) | depth(16) | seq(16)，高位优先。
         * 覆盖层约定 layer=200, transparent=1。
         */
        RENDER_API uint64_t rxMakeSortKey(uint8_t layer, uint8_t transparent, uint16_t depth,
                                          uint16_t seq);

        }  // extern "C"

    }  // namespace RT
}  // namespace Render

#endif  // RENDERX_PUBLIC_API_H
