// 点图元片段着色器（P3C4）—— 圆形裁剪，保留 alpha
//
// 与 GLSL 版（src/shader/point_p3c4.frag）对应。
struct RxFragmentIn
{
    float4 vColor;
};

fragment float4 fs_main(RxFragmentIn in [[stage_in]],
                        float2 pointCoord [[point_coord]])
{
    float2 d = pointCoord * 2.0 - 1.0;
    if (dot(d, d) > 1.0)
    {
        discard_fragment();
    }
    return in.vColor;
}
