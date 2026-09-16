/**
 * @file Mat4Tests.cpp
 * @brief 列主序 4x4 合成的契约测试
 *
 * 覆盖 rxSessionSetModelMatrix 背后的那一次乘法。两条断言是这个接口的
 * 全部要害：
 *   1. **语义顺序**：合成结果作用到点上，必须等价于「先模型、后视图」。
 *      写反了整幅画面会平移反方向、旋转绕错中心。
 *   2. **行列主序**：平移分量必须落在 m[12..14]。主序搞错时矩阵本身
 *      "看起来"没错，只是画面全错。
 */

#include <gtest/gtest.h>

#include "../src/core/mat4.h"

#include <cmath>

namespace
{
    using Render::RT::detail::kIdentity4x4;
    using Render::RT::detail::multiply4x4;

    /// 列主序平移矩阵
    void makeTranslate(float x, float y, float z, float out[16])
    {
        for (int i = 0; i < 16; ++i)
        {
            out[i] = kIdentity4x4[i];
        }
        out[12] = x;
        out[13] = y;
        out[14] = z;
    }

    /// 列主序绕 Z 轴旋转矩阵
    void makeRotateZ(float radians, float out[16])
    {
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        for (int i = 0; i < 16; ++i)
        {
            out[i] = 0.0f;
        }
        out[0] = c;
        out[1] = s;
        out[4] = -s;
        out[5] = c;
        out[10] = 1.0f;
        out[15] = 1.0f;
    }

    /// 与着色器 `uView * vec4(p, 1.0)` 等价的点变换（列主序）
    void transformPoint(const float m[16], float x, float y, float z, float out[3])
    {
        out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
        out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
        out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    }

    constexpr float kHalfPi = 1.5707963267948966f;
}  // namespace

TEST(Mat4Composition, IdentityIsNeutralOnBothSides)
{
    float translate[16];
    makeTranslate(3.0f, -4.0f, 5.0f, translate);

    float out[16];
    multiply4x4(kIdentity4x4, translate, out);
    for (int i = 0; i < 16; ++i)
    {
        EXPECT_FLOAT_EQ(translate[i], out[i]) << "单位矩阵在左侧应等价于不乘，下标 " << i;
    }

    multiply4x4(translate, kIdentity4x4, out);
    for (int i = 0; i < 16; ++i)
    {
        EXPECT_FLOAT_EQ(translate[i], out[i]) << "单位矩阵在右侧应等价于不乘，下标 " << i;
    }
}

TEST(Mat4Composition, TranslationLandsInLastColumn)
{
    // 列主序约定：平移分量在 m[12..14]。主序写错（当作行主序）时，
    // 这一条会先失败，而不是等到画面上才发现模型位置莫名其妙。
    float translate[16];
    makeTranslate(7.0f, 8.0f, 9.0f, translate);

    EXPECT_FLOAT_EQ(7.0f, translate[12]);
    EXPECT_FLOAT_EQ(8.0f, translate[13]);
    EXPECT_FLOAT_EQ(9.0f, translate[14]);

    float point[3];
    transformPoint(translate, 0.0f, 0.0f, 0.0f, point);
    EXPECT_FLOAT_EQ(7.0f, point[0]);
    EXPECT_FLOAT_EQ(8.0f, point[1]);
    EXPECT_FLOAT_EQ(9.0f, point[2]);
}

TEST(Mat4Composition, ViewTimesModelAppliesModelFirst)
{
    // 这是接口的核心语义：合成结果 == 视图 × 模型，
    // 也就是「顶点先按模型变换，再按视图变换」。
    float view[16];
    makeTranslate(10.0f, 0.0f, 0.0f, view);
    float model[16];
    makeTranslate(0.0f, 5.0f, 0.0f, model);

    float combined[16];
    multiply4x4(view, model, combined);

    // 点 (1,2,0)：先模型 → (1,7,0)，再视图 → (11,7,0)
    float point[3];
    transformPoint(combined, 1.0f, 2.0f, 0.0f, point);
    EXPECT_NEAR(11.0f, point[0], 1e-5f);
    EXPECT_NEAR(7.0f, point[1], 1e-5f);
    EXPECT_NEAR(0.0f, point[2], 1e-5f);
}

TEST(Mat4Composition, OperandOrderIsNotCommutative)
{
    // 旋转与平移不可交换：如果实现里把两个乘数写反，这里必须能发现。
    float view[16];
    makeTranslate(10.0f, 0.0f, 0.0f, view);
    float model[16];
    makeRotateZ(kHalfPi, model);

    float viewTimesModel[16];
    multiply4x4(view, model, viewTimesModel);

    // 先旋转 90°：(1,0,0) → (0,1,0)；再平移 (10,0,0) → (10,1,0)
    float p1[3];
    transformPoint(viewTimesModel, 1.0f, 0.0f, 0.0f, p1);
    EXPECT_NEAR(10.0f, p1[0], 1e-5f);
    EXPECT_NEAR(1.0f, p1[1], 1e-5f);

    float modelTimesView[16];
    multiply4x4(model, view, modelTimesView);

    // 先平移 (1,0,0) → (11,0,0)；再旋转 90° → (0,11,0)
    float p2[3];
    transformPoint(modelTimesView, 1.0f, 0.0f, 0.0f, p2);
    EXPECT_NEAR(0.0f, p2[0], 1e-5f);
    EXPECT_NEAR(11.0f, p2[1], 1e-5f);

    // 两种顺序的结果必须不同，否则这条用例没有区分力
    EXPECT_GT(std::fabs(p1[0] - p2[0]), 1.0f);
}

TEST(Mat4Composition, OutputMayAliasAnOperand)
{
    // 实现声明 out 允许与操作数同一块内存。这里刻意让 b 是旋转矩阵：
    // 若实现边读边写，a 中被覆盖的分量会污染后续行的计算，结果与
    // 非别名路径不一致——用单位矩阵做 b 是测不出来的（a*I 恒等于 a）。
    float a[16];
    makeTranslate(1.0f, 2.0f, 3.0f, a);
    float b[16];
    makeRotateZ(0.7f, b);

    float reference[16];
    multiply4x4(a, b, reference);

    multiply4x4(a, b, a);  // out 与第一个操作数别名

    for (int i = 0; i < 16; ++i)
    {
        EXPECT_FLOAT_EQ(reference[i], a[i]) << "out 与操作数别名时下标 " << i << " 被破坏";
    }
}
