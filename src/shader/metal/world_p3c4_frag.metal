// 世界空间 / P3C4 —— 保留 alpha（片段阶段）
//
// 与 GLSL 版（src/shader/world_p3c4.frag）一致。WorldPinned 管线也复用它。
#include <metal_stdlib>
using namespace metal;

struct RxFragmentIn
{
    float4 vColor;
};

fragment float4 fs_main(RxFragmentIn in [[stage_in]])
{
    return in.vColor;
}
