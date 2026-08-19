/**
 * @file render.h
 * @brief Render 模块的公共 API 头文件
 *
 * 本文件定义了渲染模块对外暴露的 C 接口，包括：
 * - 设备创建和销毁
 * - 图元管理（添加、修改、删除）
 * - 网格和实例管理
 * - 材质管理
 * - 视图设置
 * - 覆盖层和文本渲染
 * - 场景环境渲染
 * - 帧渲染
 *
 * 所有接口都使用 C 语言调用约定，便于跨语言绑定和动态库使用。
 */
#pragma once

#include "render_types.h"
#include <cstdint>

/// API 导出宏定义，支持 Windows 和 Linux/macOS 平台
#ifndef RENDER_API
    #if defined(_WIN32) || defined(_WIN64)
        #ifdef RENDER_EXPORTS
            #define RENDER_API __declspec(dllexport)
        #else
            #define RENDER_API __declspec(dllimport)
        #endif
    #elif defined(__GNUC__) || defined(__clang__)
        #ifdef RENDER_EXPORTS
            #define RENDER_API __attribute__((visibility("default")))
        #else
            #define RENDER_API
        #endif
    #else
        #define RENDER_API
    #endif
#endif

namespace render
{

    /**
     * @brief 渲染统计信息结构
     *
     * 用于查询当前帧的渲染统计数据。
     */
    struct RenderStats
    {
        uint32_t entityCount;     ///< 当前场景中的图元总数
        uint32_t visibleCount;    ///< 当前帧可见的图元数量
        uint32_t drawCallCount;   ///< 当前帧的绘制调用次数
        uint32_t triangleCount;   ///< 当前帧绘制的三角形数量
        uint32_t lineCount;       ///< 当前帧绘制的线段数量
        uint32_t pointCount;      ///< 当前帧绘制的点数量
        uint64_t gpuMemoryBytes;  ///< 当前 GPU 内存使用量（字节）
    };

    /// 渲染设备的前向声明
    typedef struct RenderDevice RenderDevice;

    extern "C"
    {

        /**
         * @brief 创建渲染设备
         *
         * @param desc 设备描述结构，包含后端类型、窗口句柄等参数
         * @return 渲染设备指针，失败返回 nullptr
         */
        RENDER_API RenderDevice* renderCreateDevice(const DeviceDesc* desc);

        /**
         * @brief 销毁渲染设备
         *
         * @param dev 要销毁的渲染设备
         */
        RENDER_API void renderDestroyDevice(RenderDevice* dev);

        /**
         * @brief 调整渲染目标尺寸
         *
         * @param dev 渲染设备
         * @param width 新的宽度（像素）
         * @param height 新的高度（像素）
         */
        RENDER_API void renderResize(RenderDevice* dev, uint32_t width, uint32_t height);

        /**
         * @brief 添加2D图元到场景
         *
         * @param dev 渲染设备
         * @param id 图元唯一标识符
         * @param vertices 顶点数据数组
         * @param vertexCount 顶点数量
         * @param type 图元类型
         * @param materialIdx 材质索引
         * @return 图元在内部的索引位置
         */
        RENDER_API uint32_t renderAddEntity(RenderDevice* dev,
            EntityId id,
            const VertexP3C3* vertices,
            uint32_t vertexCount,
            PrimitiveType type,
            uint16_t materialIdx);

        /**
         * @brief 修改已存在的2D图元
         *
         * @param dev 渲染设备
         * @param id 要修改的图元ID
         * @param vertices 新的顶点数据
         * @param vertexCount 顶点数量
         * @param type 图元类型（更新拓扑，避免图元闭合性变化后保留旧拓扑）
         * @param materialIdx 材质索引
         */
        RENDER_API void renderModifyEntity(RenderDevice* dev,
            EntityId id,
            const VertexP3C3* vertices,
            uint32_t vertexCount,
            PrimitiveType type,
            uint16_t materialIdx);

        /**
         * @brief 从场景中删除2D图元
         *
         * @param dev 渲染设备
         * @param id 要删除的图元ID
         */
        RENDER_API void renderRemoveEntity(RenderDevice* dev, EntityId id);

        /**
         * @brief 设置图元的可见性
         *
         * @param dev 渲染设备
         * @param id 图元ID
         * @param visible 可见性：0=不可见，1=可见
         */
        RENDER_API void renderSetEntityVisibility(RenderDevice* dev, EntityId id, int32_t visible);

        /**
         * @brief 批量应用图元更新
         *
         * @param dev 渲染设备
         * @param packet 更新数据包指针
         * @param packetSize 数据包大小（字节）
         */
        RENDER_API void renderApplyUpdates(RenderDevice* dev, const void* packet, uint32_t packetSize);

        /**
         * @brief 添加3D图元到渲染世界
         *
         * 将3D三角网格图元添加到 RenderWorld3D，用于统一渲染管线。
         *
         * @param dev 渲染设备
         * @param id 图元唯一标识
         * @param positions 顶点位置数组（每顶点3个float：x,y,z）
         * @param normals 顶点法线数组（每顶点3个float：nx,ny,nz）
         * @param vertices 顶点数量
         * @param indices 索引数组（每三角形3个uint32）
         * @param indicesCount 索引数量
         * @param materialIndex 材质索引
         */
        RENDER_API void renderAddEntity3D(RenderDevice* dev,
            EntityId id,
            const float* positions,
            const float* normals,
            uint32_t vertices,
            const uint32_t* indices,
            uint32_t indicesCount,
            uint16_t materialIndex);

        /**
         * @brief 从渲染世界删除3D图元
         *
         * @param dev 渲染设备
         * @param id 要删除的图元ID
         */
        RENDER_API void renderRemoveEntity3D(RenderDevice* dev, EntityId id);

        /**
         * @brief 清空3D渲染世界
         *
         * @param dev 渲染设备
         */
        RENDER_API void renderClearWorld3D(RenderDevice* dev);

        /**
         * @brief 注册3D网格到渲染系统
         *
         * @param dev 渲染设备
         * @param positions 顶点位置数组（每顶点3个float）
         * @param normals 顶点法向量数组（每顶点3个float，可为null）
         * @param indices 索引数组（可为null）
         * @param vertexCount 顶点数量
         * @param indexCount 索引数量
         * @return 网格ID，失败返回INVALID_MESH_ID
         */
        RENDER_API MeshId renderRegisterMesh(RenderDevice* dev,
            const float* positions,
            const float* normals,
            const uint32_t* indices,
            uint32_t vertexCount,
            uint32_t indexCount);

        /**
         * @brief 从渲染系统中注销3D网格
         *
         * @param dev 渲染设备
         * @param mesh 要注销的网格ID
         */
        RENDER_API void renderUnregisterMesh(RenderDevice* dev, MeshId mesh);

        /**
         * @brief 添加3D网格实例
         *
         * @param dev 渲染设备
         * @param mesh 网格ID
         * @param modelMatrix 4x4模型矩阵（列主序）
         * @param materialIdx 材质索引
         * @return 实例ID
         */
        RENDER_API uint32_t renderAddInstance(
            RenderDevice* dev, MeshId mesh, const float modelMatrix[16], uint32_t materialIdx, const float color[4]);

        /**
         * @brief 修改3D网格实例
         *
         * @param dev 渲染设备
         * @param instanceId 实例ID
         * @param modelMatrix 新的4x4模型矩阵（列主序）
         */
        RENDER_API void renderModifyInstance(RenderDevice* dev, uint32_t instanceId, const float modelMatrix[16]);

        /**
         * @brief 删除3D网格实例
         *
         * @param dev 渲染设备
         * @param instanceId 要删除的实例ID
         */
        RENDER_API void renderRemoveInstance(RenderDevice* dev, uint32_t instanceId);

        /**
         * @brief 添加材质到渲染系统
         *
         * @param dev 渲染设备
         * @param desc 材质描述结构
         * @return 材质索引
         */
        RENDER_API uint16_t renderAddMaterial(RenderDevice* dev, const MaterialDesc* desc);

        /**
         * @brief 更新已有材质
         *
         * @param dev 渲染设备
         * @param idx 材质索引
         * @param desc 新的材质描述
         */
        RENDER_API void renderUpdateMaterial(RenderDevice* dev, uint16_t idx, const MaterialDesc* desc);

        /**
         * @brief 设置2D视图参数
         *
         * @param dev 渲染设备
         * @param viewMatrix 3x3视图变换矩阵（列主序）
         * @param viewWidth 视图宽度（世界坐标单位）
         * @param viewHeight 视图高度（世界坐标单位）
         */
        RENDER_API void renderSetView2D(RenderDevice* dev, const float viewMatrix[9], float viewWidth, float viewHeight);

        /**
         * @brief 设置2D相机中心（用于 camera-relative 渲染）
         *
         * 当场景坐标值较大时（如 CAD 图纸坐标在数万量级），
         * 顶点着色器中的 viewMatrix 乘法会因 float32 精度不足导致渲染伪影
         * （线条断裂、虚线化等）。通过在细分阶段以 double 精度减去相机中心，
         * 使传入 GPU 的顶点坐标保持在相机附近的小数值范围，消除精度损失。
         *
         * @param dev 渲染设备
         * @param cx 相机中心 X 坐标（世界坐标系，double 精度）
         * @param cy 相机中心 Y 坐标（世界坐标系，double 精度）
         */
        RENDER_API void renderSetCameraCenter(RenderDevice* dev, double cx, double cy);

        /**
         * @brief 设置3D视图参数
         *
         * @param dev 渲染设备
         * @param viewMatrix 4x4视图矩阵（列主序）
         * @param projMatrix 4x4投影矩阵（列主序）
         */
        RENDER_API void renderSetView3D(RenderDevice* dev, const float viewMatrix[16], const float projMatrix[16]);

        /**
         * @brief 设置视图模式（2D/3D）
         *
         * 切换视图模式会影响背景色和渲染状态：
         * - Mode2D: 浅灰色背景，禁用深度测试
         * - Mode3D: 深蓝色背景，启用深度测试
         *
         * @param dev 渲染设备
         * @param mode 视图模式
         */
        RENDER_API void renderSetViewMode(RenderDevice* dev, ViewMode mode);

        /**
         * @brief 加载屏幕文本渲染器的字体（可选，默认字体由 renderCreateDevice 自动加载）
         *
         * @param dev 渲染设备
         * @param fontData 字体数据（TTF/OTF 二进制）
         * @param dataSize 字体数据大小（字节）
         * @param pixelHeight 字体像素高度（默认 16）
         */
        RENDER_API void renderLoadScreenFont(
            RenderDevice* dev, const void* fontData, uint32_t dataSize, float pixelHeight);

        /**
         * @brief 暂存屏幕空间文本（在 renderFrame 中统一渲染）
         *
         * 在 renderFrame 之前调用，文本将在帧末尾以屏幕像素坐标渲染。
         * 视口尺寸由渲染设备内部维护，调用方无需传入。
         *
         * @param dev 渲染设备
         * @param items 文本项数组
         * @param count 文本项数量
         */
        RENDER_API void renderSetScreenTexts(RenderDevice* dev, const ScreenTextItem* items, uint32_t count);

        /**
         * @brief 设置清屏颜色
         *
         * @param dev 渲染设备
         * @param r 红色分量（0~1）
         * @param g 绿色分量（0~1）
         * @param b 蓝色分量（0~1）
         * @param a 透明度分量（0~1）
         */
        RENDER_API void renderSetClearColor(RenderDevice* dev, float r, float g, float b, float a);

        /**
         * @brief 设置要渲染的文本列表
         *
         * @param dev 渲染设备
         * @param texts 文本项列表结构
         */
        RENDER_API void renderSetTexts(RenderDevice* dev, const TextItemList* texts);

        /**
         * @brief 提交单个叠加层图元（统一 API）
         *
         * Phase 1 引入的统一 overlay 提交入口。调用方通过 OverlayPrimitive 描述几何形态(form)、
         * 生命周期分组(group)、几何数据和样式，渲染器内部转换为顶点数据并批量渲染。
         * 渲染只认 form，清除只认 group，视觉差异走 style。
         *
         * 替代范围：renderSetPreviewLines / renderSetControlLines / renderSetSelectionBox /
         *           renderSetSelectionRect / renderSetSelectionHandles / renderSetPointMarkers
         *
         * @param dev 渲染设备
         * @param primitive 图元描述指针
         */
        RENDER_API void renderSubmitOverlay(RenderDevice* dev, const OverlayPrimitive* primitive);

        /**
         * @brief 批量提交叠加层图元（统一 API）
         *
         * 批量版本的 renderSubmitOverlay，减少多次调用的开销。
         *
         * @param dev 渲染设备
         * @param primitives 图元描述数组指针
         * @param count 图元数量
         */
        RENDER_API void renderSubmitOverlays(RenderDevice* dev, const OverlayPrimitive* primitives, uint32_t count);

        /**
         * @brief 清除所有通过统一 API 提交的叠加层图元
         *
         * @param dev 渲染设备
         */
        RENDER_API void renderClearOverlays(RenderDevice* dev);

        /**
         * @brief 按生命周期分组清除通过统一 API 提交的叠加层图元
         *
         * overlay 采用 几何形态(form) × 生命周期分组(group) 两个独立轴：
         * 渲染只认 form（几何如何生成顶点/使用哪种拓扑），清除只认 group。
         *
         * 与 renderClearOverlays 不同，此方法只清除指定分组的统一 overlay，
         * 其他分组的统一 overlay 保留。
         *
         * @param dev   渲染设备
         * @param group 要清除的 overlay 分组
         */
        RENDER_API void renderClearOverlayGroup(RenderDevice* dev, OverlayGroup group);

        /**
         * @brief 设置场景环境层（完整版本，支持像素坐标、三角形标志、深度）
         *
         * @param dev 渲染设备
         * @param vertices 顶点数据
         * @param vertexCount 顶点数量
         * @param layerOffsets 各层的顶点偏移数组
         * @param layerCount 层数
         * @param layerColors 各层的颜色数组（RGBA格式）
         * @param layerWidths 各层的线宽数组
         * @param pixelFlags 各层是否使用像素坐标的标志数组
         */
        RENDER_API void renderSetSceneEnvEx(RenderDevice* dev,
            const VertexP3C3* vertices,
            uint32_t vertexCount,
            const uint32_t* layerOffsets,
            uint32_t layerCount,
            const uint32_t* layerColors,
            const float* layerWidths,
            const bool* pixelFlags,
            const bool* triangleFlags,
            const float* zDepths);

        /**
         * @brief 设置场景环境几何（描述符直通版本）
         *
         * 直接消费 SceneEnvGeometryDesc（纯 POD，无 Engine 依赖），Renderx 内部完成
         * xy 坐标对到 VertexP3C3 的转换与层颜色填充。标尺文字不在此 API 内，继续走
         * renderSetScreenTexts。
         *
         * @param dev 渲染设备
         * @param desc 场景环境几何描述符（vertices/layers 指针在调用期间保持有效即可，
         *             Renderx 内部同步拷贝，不持有悬垂引用）
         */
        RENDER_API void renderSetSceneEnvDirect(RenderDevice* dev, const SceneEnvGeometryDesc* desc);

        /**
         * @brief 设置位图图像（用于显示图片覆盖层）
         *
         * @param dev 渲染设备
         * @param rgba RGBA像素数据
         * @param w 图像宽度
         * @param h 图像高度
         * @param tlX 左上角X坐标（世界坐标）
         * @param tlY 左上角Y坐标（世界坐标）
         * @param trX 右上角X坐标（世界坐标）
         * @param trY 右上角Y坐标（世界坐标）
         * @param blX 左下角X坐标（世界坐标）
         * @param blY 左下角Y坐标（世界坐标）
         * @param brX 右下角X坐标（世界坐标）
         * @param brY 右下角Y坐标（世界坐标）
         */
        RENDER_API void renderSetBitmap(RenderDevice* dev,
            const uint8_t* rgba,
            int32_t w,
            int32_t h,
            float tlX,
            float tlY,
            float trX,
            float trY,
            float blX,
            float blY,
            float brX,
            float brY);

        /**
         * @brief 清除位图图像
         *
         * @param dev 渲染设备
         */
        RENDER_API void renderClearBitmap(RenderDevice* dev);

        /**
         * @brief 按 entityId 添加/更新位图（多图槽位版本）
         *
         * 与 renderSetBitmap（固定 entityId=0 的单图便捷入口）不同，本接口按 entityId
         * 独立管理多张位图，供多图覆盖场景使用。entityId=0 被保留给 renderSetBitmap。
         *
         * @param dev 渲染设备
         * @param entityId 位图唯一标识（不能为 0）
         * @param rgba RGBA 像素数据
         * @param w 图像宽度
         * @param h 图像高度
         * @param tlX 左上角 X 坐标（世界坐标）
         * @param tlY 左上角 Y 坐标（世界坐标）
         * @param trX 右上角 X 坐标（世界坐标）
         * @param trY 右上角 Y 坐标（世界坐标）
         * @param blX 左下角 X 坐标（世界坐标）
         * @param blY 左下角 Y 坐标（世界坐标）
         * @param brX 右下角 X 坐标（世界坐标）
         * @param brY 右下角 Y 坐标（世界坐标）
         */
        RENDER_API void renderUpsertBitmap(RenderDevice* dev,
            uint64_t entityId,
            const uint8_t* rgba,
            int32_t w,
            int32_t h,
            float tlX,
            float tlY,
            float trX,
            float trY,
            float blX,
            float blY,
            float brX,
            float brY);

        /**
         * @brief 按 entityId 移除位图
         *
         * @param dev 渲染设备
         * @param entityId 要移除的位图标识（不能为 0）
         */
        RENDER_API void renderRemoveBitmap(RenderDevice* dev, uint64_t entityId);

        /**
         * @brief 清除所有位图（包括 renderSetBitmap 的单图槽位）
         *
         * @param dev 渲染设备
         */
        RENDER_API void renderClearBitmaps(RenderDevice* dev);

        /**
         * @brief 执行一帧渲染
         *
         * 此函数会执行完整的渲染流程，包括：
         * - 更新可见性查询
         * - 构建绘制批次
         * - 上传数据到GPU
         * - 执行绘制调用
         *
         * @param dev 渲染设备
         */
        RENDER_API void renderFrame(RenderDevice* dev);

        /**
         * @brief 读取当前帧缓冲区像素数据
         *
         * 从当前绑定的帧缓冲区读取像素数据到 CPU 内存。
         * 数据格式为 RGBA8（每像素 4 字节，行跨度由 rowPitch 返回）。
         * 注意：GL 行序为底部向上，调用方通常需要自行翻转（垂直镜像）。
         *
         * @param dev      渲染设备
         * @param x        起始 X 坐标（像素）
         * @param y        起始 Y 坐标（像素）
         * @param width    读取宽度
         * @param height   读取高度
         * @param outPixels 输出缓冲区，需至少 width * height * 4 字节
         * @param outRowPitch 输出行跨度（字节），可为 nullptr
         * @return 成功返回 1，失败返回 0
         */
        RENDER_API int renderReadPixels(RenderDevice* dev,
            uint32_t x, uint32_t y, uint32_t width, uint32_t height,
            void* outPixels, uint32_t* outRowPitch);

        /**
         * @brief 获取渲染统计信息
         *
         * @param dev 渲染设备
         * @param stats 输出参数，用于存储统计信息
         */
        RENDER_API void renderGetStats(RenderDevice* dev, RenderStats* stats);

        /**
         * @brief 获取场景中的图元数量
         *
         * @param dev 渲染设备
         * @return 图元总数
         */
        RENDER_API uint32_t renderGetEntityCount(RenderDevice* dev);

        /**
         * @brief 获取GPU内存使用量
         *
         * @param dev 渲染设备
         * @return GPU内存使用量（字节）
         */
        RENDER_API uint64_t renderGetGPUMemoryUsage(RenderDevice* dev);

        /**
         * @brief 获取原生渲染上下文
         *
         * @param dev 渲染设备
         * @return 原生上下文指针（如OpenGL的HGLRC）
         */
        RENDER_API void* renderGetNativeContext(RenderDevice* dev);

        /**
         * @brief 开始场景渲染（内部使用）
         *
         * @param dev 渲染设备
         */
        RENDER_API void renderBeginScene(RenderDevice* dev);

        /**
         * @brief 结束场景渲染（内部使用）
         *
         * @param dev 渲染设备
         */
        RENDER_API void renderEndScene(RenderDevice* dev);

        /**
         * @brief 提交单个几何图元（统一 API）
         *
         * Phase 2 引入的统一几何提交入口。调用方通过 GeometryPrimitive 描述图元类型和
         * 类型明确的描述指针，渲染器内部按 kind 分发到对应路径：
         * - Polyline / Circle / Arc / Ellipse / Image → 2D 文档几何路径
         * - Text → 文本缓存路径（TextAtlas）
         * - TriangleSoup → 3D mesh 路径（MeshManager）
         *
         * 该 API 为几何提交的统一入口，替代早期分散的 renderEmit* 系列。
         *
         * @param dev 渲染设备
         * @param primitive 图元描述指针
         */
        RENDER_API void renderSubmitGeometry(RenderDevice* dev, const GeometryPrimitive* primitive);

        /**
         * @brief 批量提交几何图元（统一 API）
         *
         * 批量版本的 renderSubmitGeometry，减少多次调用的开销。
         *
         * @param dev 渲染设备
         * @param primitives 图元描述数组指针
         * @param count 图元数量
         */
        RENDER_API void renderSubmitGeometries(RenderDevice* dev, const GeometryPrimitive* primitives, uint32_t count);

        // ==================== 离屏渲染目标（截图 / 离屏合成） ====================

        /**
         * @brief 创建离屏渲染目标
         *
         * @param dev 渲染设备
         * @param desc 渲染目标描述
         * @return 渲染目标句柄，失败返回 NullRenderTarget
         */
        RENDER_API RenderTargetHandle renderCreateRenderTarget(RenderDevice* dev, const RenderTargetDesc* desc);

        /**
         * @brief 销毁离屏渲染目标
         *
         * @param dev 渲染设备
         * @param handle 渲染目标句柄
         */
        RENDER_API void renderDestroyRenderTarget(RenderDevice* dev, RenderTargetHandle handle);

        /**
         * @brief 绑定离屏渲染目标为当前绘制目标
         *
         * @param dev 渲染设备
         * @param handle 渲染目标句柄（NullRenderTarget 表示不操作）
         */
        RENDER_API void renderBindRenderTarget(RenderDevice* dev, RenderTargetHandle handle);

        /**
         * @brief 恢复绑定离屏渲染目标之前的默认绘制目标
         *
         * @param dev 渲染设备
         */
        RENDER_API void renderBindDefaultTarget(RenderDevice* dev);

        /**
         * @brief 从离屏渲染目标回读颜色像素（RGBA8）
         *
         * @param dev 渲染设备
         * @param handle 渲染目标句柄
         * @param rgba8 输出缓冲区，容量须 >= width*height*4 字节
         * @param rowPitchBytes 每行字节数（通常 = width*4）
         */
        RENDER_API void renderReadRenderTarget(RenderDevice* dev, RenderTargetHandle handle, void* rgba8, uint32_t rowPitchBytes);

        /**
         * @brief 将当前场景渲染到离屏帧缓冲并回读为图像
         *
         * 便捷接口：内部创建一张 width×height 的离屏目标，绑定后调用
         * renderFrame 渲染当前场景（调用方需提前设置好视图矩阵/相机等），
         * 回读像素后还原默认目标并销毁离屏目标。
         * 像素原点在左下角（与 GL 一致），调用方按需上下翻转。
         *
         * @param dev 渲染设备
         * @param width 输出宽度（像素）
         * @param height 输出高度（像素）
         * @param rgba8 输出缓冲区，容量须 >= width*height*4 字节
         * @param outRowPitch 输出每行字节数（= width*4），可为 nullptr
         * @return 成功返回 true
         *
         * @note 该接口仅对 2D（Renderx）场景有效；3D 视图请使用其专属离屏捕获接口。
         */
        RENDER_API bool renderCaptureFrame(
            RenderDevice* dev, uint32_t width, uint32_t height, void* rgba8, uint32_t* outRowPitch);
    }

}  // namespace render
