/**
 * @file bitmapRenderer.h
 * @brief 位图渲染器类定义
 *
 * 负责把多张 RGBA 位图上传为 GPU 纹理，并以带纹理的四边形绘制到
 * 2D 场景中。与 TextAtlas 同属"纹理四边形"渲染模式，但位图是
 * 按 entityId 独立管理的纹理（而非图集），四角的几何由调用方以
 * 世界坐标指定（支持斜切/拉伸）。
 *
 * 设计要点：
 *  - 按 entityId 增删查（多图支持），entityId 复用场景图元 ID，
 *    与增量渲染 / 全量重建的生命周期保持一致。
 *  - 像素数据在 set() 调用期间同步上传，不持有调用方指针。
 *  - renderBeginScene 全量重建时调用 clear() 清空所有位图。
 */
#pragma once

#include "render/RenderTypes.h"
#include "../rhi/rhiDevice.h"
#include <vector>
#include <cstdint>

namespace Render::core
{

    /**
     * @brief 位图渲染器
     *
     * 管理多张 RGBA 位图的纹理上传与世界坐标四边形绘制。
     */
    class BitmapRenderer
    {
    public:
        /**
         * @brief 初始化位图渲染器
         *
         * 创建内部顶点缓冲和位图渲染管线。
         *
         * @param device RHI 设备指针
         * @return 初始化是否成功
         */
        bool initialize(RHI::IDevice* device);

        /**
         * @brief 关闭并释放所有资源
         */
        void shutdown();

        /**
         * @brief 设置（新增或更新）一个位图实体
         *
         * 上传 RGBA 像素到纹理，并按世界坐标四角构建渲染四边形。
         * 坐标系约定（与 Ut::Vec2d / SyImage 一致）：
         *   - topLeft/topRight 为上边，bottomLeft/bottomRight 为下边；
         *   - Y 轴向上为正。
         *
         * @param entityId 关联的图元 ID（同一 ID 重复调用即更新）
         * @param rgba     RGBA8 像素数据（w*h*4 字节，仅在调用期间有效）
         * @param w        图像宽度（像素）
         * @param h        图像高度（像素）
         * @param corners  四角世界坐标（8 floats: TL,TR,BL,BR 的 x,y）
         */
        void set(uint64_t entityId, const uint8_t* rgba, int32_t w, int32_t h, const float corners[8]);

        /**
         * @brief 按图元 ID 移除位图（释放对应纹理）
         *
         * @param entityId 要移除的图元 ID
         */
        void remove(uint64_t entityId);

        /**
         * @brief 清空所有位图（释放全部纹理）
         */
        void clear();

        /**
         * @brief 当前位图数量
         */
        size_t count() const
        {
            return m_bitmaps.size();
        }

        /**
         * @brief 是否有有效位图可绘制
         */
        bool hasBitmap() const
        {
            return !m_bitmaps.empty();
        }

        /**
         * @brief 渲染所有位图（世界坐标四边形，camera-relative）
         *
         * @param device       RHI 设备
         * @param viewMatrix   3x3 视图矩阵（世界→NDC）
         * @param cameraCenter 相机中心（世界坐标 double[2]）
         */
        void render(RHI::IDevice* device, const float viewMatrix[9], const double cameraCenter[2]);

    private:
        /// 位图顶点结构：位置(3) + 纹理坐标(2)，对应 P3T2
        struct BitmapVertex
        {
            float px, py, pz;
            float u, v;
        };

        /// 单个位图实体条目
        struct BitmapEntry
        {
            uint64_t entityId = 0;
            RHI::TextureHandle texture = RHI::NullHandle;
            int32_t width = 0;
            int32_t height = 0;
            float corners[8] = {};
        };

        /// 在 m_bitmaps 中查找 entityId 的下标，未找到返回 SIZE_MAX
        size_t findIndex(uint64_t entityId) const;

        /// RHI 设备指针
        RHI::IDevice* m_device = nullptr;
        /// 顶点缓冲（单四边形 6 顶点，逐张位图复用）
        RHI::BufferHandle m_vertexBuffer = RHI::NullHandle;
        /// 位图渲染管线
        RHI::PipelineHandle m_pipeline = RHI::NullHandle;
        /// 位图条目集合（保持插入顺序，保证绘制顺序稳定）
        std::vector<BitmapEntry> m_bitmaps;
    };

}  // namespace render::core
