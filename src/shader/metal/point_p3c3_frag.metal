// 点图元片段着色器（P3C3）—— 把方形 sprite 裁成圆形，不透明输出
//
// 与 GLSL 版（src/shader/point_p3c3.frag）对应。GLSL 读 gl_PointCoord，
// MSL 对应 [[point_coord]]（同样是 [0,1]^2）。discard 对应 discard_fragment()。
struct RxFragmentIn
{
    float3 vColor;
};

fragment float4 fs_main(RxFragmentIn in [[stage_in]],
                        float2 pointCoord [[point_coord]])
{
    float2 d = pointCoord * 2.0 - 1.0;
    if (dot(d, d) > 1.0)
    {
        discard_fragment();
    }
    return float4(in.vColor, 1.0);
}
