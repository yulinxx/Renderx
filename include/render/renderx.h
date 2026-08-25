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
#define RENDERX_ABI_VERSION_MINOR 1
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
            /**
             * 几何仓已扩容，底层缓冲句柄被替换。
             *
             * 这**不是失败**——分配是成功的，但调用方手里所有旧
             * GeometryBlock 的 `buffer` 字段都已失效，必须用
             * rxGeometryStoreGetBuffer 重新取一次。
             * 之所以用返回码显式告知而不是静默替换：静默替换会让调用方
             * 拿着旧句柄提交，表现为「一部分图元突然不见了」。
             */
            ErrorGeometryStoreGrown = 1,
        };

        // ==================== 句柄 ====================
        //
        // enum class : uint64_t —— 8 字节 C 布局 + 编译期类型隔离。
        // 0 恒为无效值。

        enum class RuntimeHandle : uint64_t { Invalid = 0 };
        enum class SurfaceHandle : uint64_t { Invalid = 0 };
        enum class SessionHandle : uint64_t { Invalid = 0 };
        enum class BufferHandle : uint64_t { Invalid = 0 };
        enum class TextureHandle : uint64_t { Invalid = 0 };
        /// 持久几何仓：可增量更新的顶点/索引存储（见「增量渲染」一节）
        enum class GeometryStoreHandle : uint64_t { Invalid = 0 };
        /// 保留式绘制列表：DLL 侧持有并复用的 DrawCommand 集合
        enum class DrawListHandle : uint64_t { Invalid = 0 };

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

        /**
         * @brief 渲染空间：决定图元如何响应平移与缩放
         *
         * 三档语义互不重叠，标记类图元必须按需求选对，
         * 不要在业务层用「每帧按当前缩放反算顶点」来模拟：
         * 那会把渲染策略散进每个调用点，且三个后端难以保持一致。
         */
        enum class RenderSpace : uint8_t
        {
            /// 跟随平移，跟随缩放。顶点经 viewMatrix 变换。常规图元。
            World = 0,
            /// 不跟随平移，不跟随缩放。顶点直接是像素坐标。HUD、标尺、屏幕角标。
            Screen = 1,
            /**
             * 跟随平移，**不**跟随缩放。顶点 = 世界锚点 + 像素偏移。
             *
             * 用于场景内的定尺寸标记：箭头、符号、标注框、引线端点。
             * 需配合 VertexFormat::P3O2C4；换算在顶点着色器内完成
             * （clip.xy += offset * 2/viewport * clip.w），
             * 因此缩放不影响屏幕尺寸。
             *
             * 拾取判定必须用同一公式（锚点投影后再加像素偏移），
             * 否则视觉与命中区会随缩放错位。
             */
            WorldPinned = 2,
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
            /// 世界锚点 float3 + 像素偏移 float2 + 颜色 float4，36 字节。
            /// 专用于 RenderSpace::WorldPinned。
            P3O2C4 = 4,
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
            case VertexFormat::P3O2C4: return 36;
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
            /// 世界锚点 + 像素偏移的定尺寸标记（P3O2C4）。
            /// 点标记继续用 ScreenPoint 系列：点本身就是像素尺寸。
            WorldPinnedLine = 13,
            WorldPinnedTri = 14,
            Count = 15,
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
         * DLL 不依赖宿主的日志库。此前 17 个源文件直接
         * #include "Log/SyLogger.h"，这是 RenderX 无法独立复用的原因；
         * 现在整个 Renderx 目录下已无该头文件的任何引用，
         * 日志一律通过本回调交回宿主。message 仅在回调期间有效。
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
            /// 顶点数据在 vertexBuffer 中的**字节**偏移。
            /// 与 TransientAlloc::offset 同一坐标系，可直接原样填入。
            uint32_t vertexOffset;
            uint32_t vertexCount;
            /// 索引数据在 indexBuffer 中的**字节**偏移
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

        // ==================== 增量渲染：持久几何仓 ====================
        //
        // 瞬态环（rxSessionAllocTransient）解决的是「每帧都变」的数据：
        // 预览线、橡皮筋、覆盖层。它的代价是每帧全量重传。
        //
        // 但 CAD 场景的绝大多数图元**帧间不变**。10 万条线段里改动一条，
        // 用瞬态环意味着重传 10 万条。几何仓就是为这种情况存在的：
        // 顶点常驻显存，编辑只重写变化的那一块。
        //
        // 仓内按块（Block）细分配，块的释放走空闲表并合并相邻空洞。
        // 写入不立即上传：脏区间累积到帧末合并后一次性提交，
        // 避免「改了 1 万个小块 = 1 万次 writeBuffer」。

        struct GeometryStoreDesc
        {
            /// 初始容量（字节）。0 表示用默认值 4MB。
            uint64_t initialBytes;
            /// 增长上限（字节）。0 表示不限（受设备显存约束）。
            uint64_t maxBytes;
            /// 分配粒度（字节），会向上取整到 16 的倍数。0 表示默认 256。
            /// 粒度越大碎片越多，但空闲表越短、合并越快。
            uint32_t granularity;
            /// 0/1。是否用于索引数据（影响 RHI 的 usage 标记）。
            uint8_t forIndices;
            uint8_t _pad0[3];
        };
        static_assert(sizeof(GeometryStoreDesc) == 24, "GeometryStoreDesc ABI size changed");

        /**
         * @brief 几何仓中的一块
         *
         * `buffer` + `offset` 可直接填进 DrawCommand 的 vertexBuffer/vertexOffset。
         *
         * ⚠️ 仓扩容时底层缓冲会被替换，此时**所有已发出的 GeometryBlock 中的
         * `buffer` 字段都会失效**。调用方必须在 rxGeometryAlloc 返回
         * `ErrorGeometryStoreGrown` 后用 rxGeometryStoreGetBuffer 重新取一次句柄。
         * 这个约定是显式的——静默替换句柄会让调用方拿着旧句柄画出空白。
         */
        struct GeometryBlock
        {
            BufferHandle buffer;
            /// 块标识。释放与写入都用它，不要用 offset 当身份（扩容/整理后会变）。
            uint64_t id;
            uint32_t offset;
            uint32_t sizeBytes;
        };
        static_assert(sizeof(GeometryBlock) == 24, "GeometryBlock ABI size changed");

        struct GeometryStoreStats
        {
            uint64_t capacityBytes;
            /// 已分配给块的字节数（含粒度对齐产生的内部浪费）
            uint64_t usedBytes;
            /// 空闲表中最大连续空洞，用于判断是否需要整理
            uint64_t largestFreeBytes;
            uint32_t blockCount;
            uint32_t freeRangeCount;
            /// 本帧因写入而排队的脏字节数（合并后）
            uint64_t dirtyBytesThisFrame;
            /// 累计扩容次数。频繁扩容说明 initialBytes 给小了。
            uint32_t growCount;
            uint32_t _pad0;
        };
        static_assert(sizeof(GeometryStoreStats) == 48, "GeometryStoreStats ABI size changed");

        // ==================== 增量渲染：保留式绘制列表 ====================
        //
        // 每帧重建 10 万条 DrawCommand 的 CPU 开销与重传顶点同量级，
        // 而其中绝大多数条目帧间完全相同。DrawList 让 DLL 持有这份列表，
        // 调用方只 upsert 变化的槽位。
        //
        // DLL 在提交时做三件调用方做不了、或做不划算的事：
        //   1. 视口剔除（用条目自带的 AABB，不需要调用方每帧再传一份）
        //   2. 按 sortKey 排序——列表只在有改动时重排，不是每帧
        //   3. 合批：相邻条目若状态相同且顶点区间连续，合成一次 draw
        //
        // 「状态相同 + 顶点连续」在几何仓里是常态，因为同类图元的块通常
        // 挨着分配。这正是几何仓与绘制列表要配合使用的原因。

        struct DrawListDesc
        {
            /// 预留槽位数。可后续增长，预留只是避免早期反复搬迁。
            uint32_t initialCapacity;
            /// 0/1。是否启用合批。关闭便于排查「某图元没画出来」是否合批所致。
            uint8_t enableMerging;
            /// 0/1。是否启用基于条目 AABB 的剔除。
            uint8_t enableCulling;
            uint8_t _pad0[2];
        };
        static_assert(sizeof(DrawListDesc) == 8, "DrawListDesc ABI size changed");

        struct DrawListStats
        {
            /// 列表中有效条目数
            uint32_t entryCount;
            /// 上一次提交中通过剔除的条目数
            uint32_t visibleCount;
            /// 上一次提交合批后实际发出的 draw 数。
            /// visibleCount / drawCallCount 就是合批率。
            uint32_t drawCallCount;
            /// 排序发生的次数。若每帧都在涨，说明调用方每帧都在改 sortKey。
            uint32_t sortCount;
            uint64_t capacityBytes;
        };
        static_assert(sizeof(DrawListStats) == 24, "DrawListStats ABI size changed");

        struct FrameStats
        {
            uint32_t drawCallCount;
            uint32_t triangleCount;
            uint32_t lineCount;
            uint32_t pointCount;
            uint32_t pipelineSwitches;
            /// 被剔除（未提交给 GPU）的命令数。与 drawCallCount 一起看
            /// 才能判断剔除是否真的在起作用。
            uint32_t culledCommandCount;
            /// 合批省下的 draw 数：合批前的可见条目数 - 实际 draw 数
            uint32_t mergedDrawCount;
            uint32_t _pad0;
            uint64_t transientBytesUsed;
            /// 本帧几何仓实际上传的字节数（脏区合并后）。
            /// 增量渲染是否生效，看的就是这个值是否远小于顶点总量。
            uint64_t geometryUploadBytes;
            uint64_t gpuMemoryBytes;
        };
        static_assert(sizeof(FrameStats) == 56, "FrameStats ABI size changed");

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

        /**
         * @brief 创建自定义管线，返回管线索引
         *
         * 返回值是索引而不是句柄：DrawCommand::pipelineIndex 是 uint16，
         * 绘制路径每帧要用它做数万次查表，索引可以直接下标寻址。
         * 0 表示创建失败（0 恒为「让 Runtime 按 DrawCommand 自行解析」）。
         */
        RENDER_API uint16_t rxPipelineCreate(RuntimeHandle runtime, const PipelineDesc* desc);
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

        // ---------- 持久几何仓：增量更新的顶点/索引存储 ----------
        //
        // 典型用法（10 万条线段的场景，编辑一条）：
        //
        //   GeometryStoreHandle store = rxGeometryStoreCreate(runtime, &storeDesc);
        //   // 建场景：每个图元一块
        //   for (每个图元) {
        //       GeometryBlock block{};
        //       rxGeometryAlloc(store, bytes, &block);
        //       rxGeometryWrite(store, block.id, 0, bytes, vertices);
        //       记录 block;  // 之后靠它做增量更新
        //   }
        //   // 编辑一条：只重写那一块，其余 99999 条一个字节都不动
        //   rxGeometryWrite(store, block.id, 0, bytes, newVertices);

        RENDER_API GeometryStoreHandle rxGeometryStoreCreate(RuntimeHandle runtime,
                                                            const GeometryStoreDesc* desc);
        RENDER_API void rxGeometryStoreDestroy(RuntimeHandle runtime, GeometryStoreHandle store);

        /// 取仓当前的底层缓冲句柄。扩容后必须重新调用（见 ErrorGeometryStoreGrown）。
        RENDER_API BufferHandle rxGeometryStoreGetBuffer(RuntimeHandle runtime,
                                                        GeometryStoreHandle store);

        /**
         * @brief 在仓内分配一块
         *
         * @return Ok；`ErrorGeometryStoreGrown` 表示分配成功但底层缓冲已被替换，
         *         调用方需刷新此前持有的所有 GeometryBlock::buffer；
         *         `ErrorOutOfMemory` 表示达到 maxBytes 上限。
         */
        RENDER_API RxResult rxGeometryAlloc(RuntimeHandle runtime, GeometryStoreHandle store,
                                           uint64_t sizeBytes, GeometryBlock* out);

        /**
         * @brief 写入块内数据（增量更新的核心）
         *
         * 只标记脏区间，不立即上传。脏区间在帧末（或 rxGeometryFlush）合并后
         * 一次性提交，因此「改 1 万个小块」不会变成 1 万次 GPU 传输。
         *
         * @param blockId  rxGeometryAlloc 返回的 GeometryBlock::id
         * @param byteOffset 块内偏移
         */
        RENDER_API RxResult rxGeometryWrite(RuntimeHandle runtime, GeometryStoreHandle store,
                                           uint64_t blockId, uint32_t byteOffset,
                                           uint32_t sizeBytes, const void* data);

        /// 释放一块。空闲表会与相邻空洞合并，避免碎片累积。
        RENDER_API RxResult rxGeometryFree(RuntimeHandle runtime, GeometryStoreHandle store,
                                          uint64_t blockId);

        /// 主动把累积的脏区间刷到 GPU。正常情况下不需要调用——
        /// Session 在提交前会自动刷；只有在帧外批量建场景时才需要。
        RENDER_API RxResult rxGeometryFlush(RuntimeHandle runtime, GeometryStoreHandle store);

        RENDER_API RxResult rxGeometryStoreGetStats(RuntimeHandle runtime, GeometryStoreHandle store,
                                                   GeometryStoreStats* out);

        // ---------- 保留式绘制列表 ----------
        //
        // 与几何仓配合使用：几何仓免掉重传顶点，绘制列表免掉重建命令。
        //
        //   DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
        //   rxDrawListUpsert(list, slot, &command, aabb);   // 只在图元变化时调
        //   ...
        //   rxSessionSubmitDrawList(session, list, viewBounds);  // 每帧一行

        RENDER_API DrawListHandle rxDrawListCreate(RuntimeHandle runtime, const DrawListDesc* desc);
        RENDER_API void rxDrawListDestroy(RuntimeHandle runtime, DrawListHandle list);

        /**
         * @brief 写入/更新一个槽位
         *
         * @param slot 调用方自行分配的槽号，用它把渲染条目关联回业务实体。
         *             槽号不必连续；列表按需增长。
         * @param aabb 世界空间 (minX, minY, maxX, maxY)，用于 DLL 侧剔除。
         *             传 nullptr 表示该条目永不被剔除（覆盖层通常如此）。
         */
        RENDER_API RxResult rxDrawListUpsert(RuntimeHandle runtime, DrawListHandle list,
                                             uint32_t slot, const DrawCommand* command,
                                             const float aabb[4]);

        /// 移除一个槽位。槽位可被后续 upsert 复用。
        RENDER_API RxResult rxDrawListRemove(RuntimeHandle runtime, DrawListHandle list,
                                             uint32_t slot);

        /// 清空全部条目，保留已分配容量。
        RENDER_API RxResult rxDrawListClear(RuntimeHandle runtime, DrawListHandle list);

        RENDER_API RxResult rxDrawListGetStats(RuntimeHandle runtime, DrawListHandle list,
                                               DrawListStats* out);

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

        /**
         * @brief 提交一个保留式绘制列表（增量渲染的每帧入口）
         *
         * 与 rxSessionSubmit 的区别：命令由 DLL 持有，调用方不必每帧重建。
         * DLL 内部依次做：几何仓脏区刷写 → AABB 剔除 → 按需排序 → 合批 → 绘制。
         *
         * @param viewBounds 世界空间 (minX, minY, maxX, maxY)。传 nullptr 关闭剔除。
         *
         * 同一帧内可以既提交绘制列表（常驻场景）又调 rxSessionSubmit
         * （覆盖层、预览线等每帧都变的内容），两者的 sortKey 在各自提交内排序。
         */
        RENDER_API RxResult rxSessionSubmitDrawList(SessionHandle session, DrawListHandle list,
                                                    const float viewBounds[4]);

        /// 结束并呈现本帧（GL: swapBuffers, Metal: presentDrawable, VK: queuePresent）
        RENDER_API RxResult rxSessionEndFrame(SessionHandle session);

        RENDER_API RxResult rxSessionQueryVisibility(SessionHandle session, const float* aabbs,
                                                     uint32_t aabbCount, const float viewBounds[4],
                                                     VisibilityResult* out);
        RENDER_API RxResult rxSessionGetStats(SessionHandle session, FrameStats* out);

        /**
         * @brief 读回当前后备缓冲的像素（视图导出/截图）
         *
         * 必须在 rxSessionEndFrame **之前**调用——EndFrame 之后后备缓冲已交给
         * 呈现引擎，内容不再保证有效。
         *
         * 输出恒为 RGBA8、**左上原点**、逐行紧凑（rowPitch = width * 4）。
         * 各后端的原生原点不一致（GL 是左下），翻转在 DLL 内完成，
         * 这样调用方不需要知道当前跑的是哪个后端。
         *
         * @param x,y      读取区域左上角（像素，左上原点）
         * @param outBytes 至少 width * height * 4 字节
         * @return ErrorInvalidArgument 表示区域越界或缓冲过小
         */
        RENDER_API RxResult rxSessionReadPixels(SessionHandle session, uint32_t x, uint32_t y,
                                                uint32_t width, uint32_t height, void* outBytes,
                                                uint64_t outByteCapacity);

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
