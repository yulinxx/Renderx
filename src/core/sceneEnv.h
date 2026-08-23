/**
 * @file scene_env.h
 * @brief 场景环境渲染类定义
 *
 * SceneEnv 负责渲染场景环境元素，包括：
 * - 网格背景（Grid）
 * - 参考线
 * - 图层分隔线
 * - 背景填充区域
 *
 * 支持多层渲染，每层可以有不同的颜色和线宽。
 */
#pragma once

#include "render/RenderTypes.h"
#include "../rhi/rhiDevice.h"
#include <vector>
#include <cstdint>

namespace Render::core
{

    /**
     * @brief 场景环境渲染类
     *
     * 管理场景环境的几何数据和渲染状态，支持多层渲染。
     */
    class SceneEnv
    {
    public:
        /**
         * @brief 初始化场景环境渲染器
         *
         * @param device RHI设备指针
         * @return 初始化是否成功
         */
        bool initialize(RHI::IDevice* device);

        /**
         * @brief 关闭并释放所有资源
         */
        void shutdown();

        /**
         * @brief 设置场景环境几何数据（完整版本，支持像素坐标和深度排序）
         *
         * @param vertices 顶点数据
         * @param vertexCount 顶点数量
         * @param layerOffsets 各层的顶点偏移数组
         * @param layerCount 层数
         * @param layerColors 各层的颜色数组（RGBA格式，每个通道8位）
         * @param lineWidths 各层的线宽数组
         * @param pixelFlags 各层是否使用像素坐标的标志数组
         * @param triangleFlags 各层是否作为三角形渲染的标志数组
         * @param zDepths 各层的深度值数组，用于排序渲染顺序
         */
        void setGeometryEx(const VertexP3C3* vertices,
            uint32_t vertexCount,
            const uint32_t* layerOffsets,
            uint32_t layerCount,
            const uint32_t* layerColors,
            const float* lineWidths,
            const bool* pixelFlags,
            const bool* triangleFlags,
            const float* zDepths);

        /**
         * @brief 设置场景环境几何数据（描述符直通版本）
         *
         * 直接消费 SceneEnvGeometryDesc（纯 POD），内部完成 xy 坐标对到
         * VertexP3C3 的转换与层颜色填充。标尺文字不进此接口。
         *
         * @param desc 场景环境几何描述符（layers/vertices 指针在本调用同步拷贝，
         *             之后不再持有）
         */
        void setGeometryDirect(const SceneEnvGeometryDesc* desc);

        /**
         * @brief 渲染场景环境（简化版本）
         *
         * @param device RHI设备指针
         * @param viewMatrix 3x3视图矩阵
         */
        void render(RHI::IDevice* device, const float viewMatrix[9]);

        /**
         * @brief 渲染场景环境（完整版本，支持像素坐标）
         *
         * @param device RHI设备指针
         * @param viewMatrix 3x3视图矩阵
         * @param viewportWidth 视口宽度（像素）
         * @param viewportHeight 视口高度（像素）
         */
        void render(RHI::IDevice* device, const float viewMatrix[9], uint32_t viewportWidth, uint32_t viewportHeight);

    private:
        /**
         * @brief 环境层结构
         *
         * 表示场景环境中的一个渲染层。
         */
        struct EnvLayer
        {
            uint32_t firstVertex;  ///< 第一个顶点的索引
            uint32_t vertexCount;  ///< 顶点数量
            float color[4];        ///< 渲染颜色 RGBA
            float lineWidth;       ///< 线宽（像素）
            float zDepth;          ///< 深度值，用于排序渲染顺序
            bool asTriangles;      ///< 是否作为三角形渲染（否则作为线渲染）
            bool usePixelCoords;   ///< 是否使用像素坐标（否则使用世界坐标）
        };

        /// 顶点数据数组
        std::vector<VertexP3C3> m_vertices;
        /// 环境层数组
        std::vector<EnvLayer> m_layers;

        /// RHI设备指针
        RHI::IDevice* m_device = nullptr;
        /// 顶点缓冲区
        RHI::BufferHandle m_vertexBuffer = RHI::NullHandle;
        /// 线渲染管线
        RHI::PipelineHandle m_linePipeline = RHI::NullHandle;
        /// 三角形渲染管线
        RHI::PipelineHandle m_trianglePipeline = RHI::NullHandle;

        /// 是否有脏数据需要上传
        bool m_dirty = true;
    };

}  // namespace Render::core
