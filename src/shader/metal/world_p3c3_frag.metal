// 世界空间 / P3C3 —— 不透明输出（片段阶段）
//
// 与 GLSL 版（src/shader/world_p3c3.frag）一致。它同时服务 world_p3c3 与
// screen_p3c3 两条管线（后者只换顶点着色器），因此这里不引入任何空间概念。
//
// 输入结构体名可以与顶点侧不同，MSL 按**字段名**匹配 varying；
// position 在内插阶段由 [[position]] 提供，片段不需要它。
struct RxFragmentIn
{
    float3 vColor;
};

fragment float4 fs_main(RxFragmentIn in [[stage_in]])
{
    return float4(in.vColor, 1.0);
}
