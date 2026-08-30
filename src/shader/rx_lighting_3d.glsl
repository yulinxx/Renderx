// ============================================================================
// rx_lighting_3d.glsl —— 3D 光照参数块（per-pass）
//
// **本文件是唯一声明处**，由 EmbedShaders.cmake 在构建期展开。
// 注意：本文件内不得出现 include 指令的字面写法，连注释里也不行。
//
// 与 pushConstant 的分工：
//   - PushConstants（binding 0）是 per-draw 的，每次换管线/材质都重推，
//     因此只放视图矩阵、视口、点尺寸、材质三色这些小量数据（上限 128 字节）。
//   - FrameUniforms（binding 1）是 per-pass 的，一帧写一次 160 字节。
//     光照有三个方向光加环境项，塞不进 128 字节的 pushConstant 预算，
//     而且它每帧只变一次，按 per-draw 重推是纯浪费。
//
// 字段顺序与类型必须与公共 ABI 的 Render::Lighting3DDesc 逐字节一致
// （include/render/renderx.h，C++ 侧有 static_assert(sizeof == 160) 锁定）。
//
// std140 偏移（DirectionalLight3D 按结构体对齐到 16 字节，尺寸 32 字节）：
//   uAmbientColor      0..11
//   uAmbientEnabled    12..15
//   uAmbientIntensity  16..19
//   uDoubleSided       20..23
//   uSpecularEnabled   24..27
//   uSpecularIntensity 28..31
//   uKeyLight          32..63
//   uFillLight         64..95
//   uRimLight          96..127
//   uViewPos           128..139
//   uMinBrightness     140..143
//   uExposure          144..147
//   uPad0              148..159
// 合计 160 字节。
//
// 块名必须是 FrameUniforms：GL 后端按名字解析 block index
// （见 rxInternal.h 的 kFrameUniformBlockName）。
// ============================================================================

struct RxDirectionalLight
{
    /// 指向光源的单位向量（世界空间）。宿主负责归一化。
    vec3 direction;
    /// 0 表示该光源整条支路跳过
    uint enabled;
    vec3 color;
    float intensity;
};

layout(std140) uniform FrameUniforms
{
    vec3 uAmbientColor;
    uint uAmbientEnabled;
    float uAmbientIntensity;
    /// 双面光照：背面把法线翻转，而不是变黑。开放曲面（未闭合网格）需要它。
    uint uDoubleSided;
    uint uSpecularEnabled;
    float uSpecularIntensity;

    RxDirectionalLight uKeyLight;
    RxDirectionalLight uFillLight;
    RxDirectionalLight uRimLight;

    /// 相机世界坐标。由宿主提供而非从 uView 反解：uView 是 proj*view 的
    /// 合并矩阵，透视投影下无法稳定地从中恢复眼点。
    vec3 uViewPos;
    /// 亮度下限，避免完全背光的面纯黑到看不出形状
    float uMinBrightness;
    /// 线性曝光系数，作用在最终颜色上
    float uExposure;
    uvec3 uPad0;
};
