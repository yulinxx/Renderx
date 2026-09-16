/**
 * @file mat4.h
 * @brief 列主序 4x4 矩阵的最小工具集（Renderx 内部）
 *
 * 只为「视图矩阵 × 模型矩阵」这一处合成而存在：Renderx 没有通用数学库，
 * 也不需要——矩阵运算的主战场在着色器里，CPU 侧只有这一处乘法
 * （见 rxSessionSetModelMatrix）。
 *
 * 单独成文而不是塞进 rxSession.cpp 的匿名命名空间，是为了能被单元测试直接
 * 覆盖：矩阵乘法最容易出的错是**两个操作数的顺序**（先模型后视图）与**行列
 * 主序**不符，而这类错误在画面上的表现是「模型出现在莫名其妙的位置」，
 * 从渲染结果反查代价很高。
 */
#pragma once

namespace Render::RT::detail
{
    /// 单位矩阵（列主序）
    inline constexpr float kIdentity4x4[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    /**
     * @brief 列主序 4x4 相乘：out = a * b
     *
     * 列主序下 m[col * 4 + row]，因此 out[col][row] = Σ_k a[k][row] * b[col][k]。
     * 这与着色器里 `uView * vec4(aPos, 1.0)` 是同一套约定：把 out 直接当 uView
     * 用，语义就是「先按 b 变换，再按 a 变换」——即 uView * uModel 的实际含义。
     *
     * out 允许与 a / b 指向同一块内存（先在栈上算完再写回），
     * 避免调用方为了安全再复制一份。
     */
    inline void multiply4x4(const float a[16], const float b[16], float out[16])
    {
        float result[16];
        for (int col = 0; col < 4; ++col)
        {
            for (int row = 0; row < 4; ++row)
            {
                result[col * 4 + row] = a[row] * b[col * 4] + a[4 + row] * b[col * 4 + 1] +
                                        a[8 + row] * b[col * 4 + 2] + a[12 + row] * b[col * 4 + 3];
            }
        }
        for (int i = 0; i < 16; ++i)
        {
            out[i] = result[i];
        }
    }
}  // namespace Render::RT::detail
