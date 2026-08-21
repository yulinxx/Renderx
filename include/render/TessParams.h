/**
 * @file TessParams.h
 * @brief 共享的 tessellation（离散化）参数与算法
 *
 * 全量提交路径（render_c_api_frame.cpp 的 tessellate* 函数）与
 * 增量提交路径（EntityToVertices.cpp 的 entityToVertices）共用本头文件，
 * 保证圆 / 弧 / 椭圆的离散化精度完全一致。
 *
 * 统一参数：
 *   - 圆：64 段（2 的幂，对 GPU 友好）
 *   - 弧：动态 max(8, min(128, int(angleRange * 20.0)))
 *   - 椭圆：完整 64 段；椭圆弧按角度比例缩放，下限 8
 */
#pragma once

namespace Render
{
    namespace tess
    {
        /// 圆与完整椭圆的离散化段数（2 的幂，对 GPU 友好）
        constexpr int kCircleSegments = 64;

        /// 圆弧动态分段下限
        constexpr int kArcMinSegments = 8;
        /// 圆弧动态分段上限
        constexpr int kArcMaxSegments = 128;

        /// 共享 PI 常量，确保两条路径三角运算使用相同精度
        constexpr double kPi = 3.14159265358979323846;

        /**
         * @brief 根据圆弧角度范围动态计算分段数
         *
         * 算法：max(8, min(128, int(angleRange * 20.0)))。
         * 全量路径与增量路径共用，保证圆弧离散化结果一致。
         *
         * @param angleRange 圆弧角度跨度（弧度）
         * @return 分段数，区间 [8, 128]
         */
        inline int arcSegments(double angleRange)
        {
            int segs = static_cast<int>(angleRange * 20.0);
            if (segs < kArcMinSegments)
            {
                segs = kArcMinSegments;
            }
            if (segs > kArcMaxSegments)
            {
                segs = kArcMaxSegments;
            }
            return segs;
        }

        /**
         * @brief 根据椭圆弧角度范围动态计算分段数
         *
         * 以完整椭圆段数（kCircleSegments）为基准按角度比例缩放，下限 8。
         * 完整椭圆（angleRange = 2π）结果为 kCircleSegments（64），与圆一致。
         *
         * @param angleRange 椭圆弧角度跨度（弧度）
         * @return 分段数，区间 [8, kCircleSegments]
         */
        inline int ellipseSegments(double angleRange)
        {
            int segs = static_cast<int>(angleRange / (2.0 * kPi) * kCircleSegments);
            if (segs < kArcMinSegments)
            {
                segs = kArcMinSegments;
            }
            if (segs > kCircleSegments)
            {
                segs = kCircleSegments;
            }
            return segs;
        }
    }  // namespace tess
}  // namespace Render
