# ============================================================================
# EmbedShaders.cmake —— 将 shader 源/字节码嵌入生成的 C++ 源文件
#
# 用途：以脚本模式（cmake -P）运行，把一批 shader 文件转成一个 .cpp，
# 内容为字节数组 + 索引表。构建后 shader 存在于 DLL 二进制内部，
# 运行时不再需要任何文件 IO。
#
# 为什么必须这么做（旧方案的三个实际故障）：
#   1. shader 是运行时从「可执行文件所在目录」读取的文本
#      （见 Docs/Mac渲染.md §4「全黑视口」）。DLL 因此依赖运行目录布局，
#      在 macOS .app bundle / iOS 下极易失效，症状是整个视口全黑。
#   2. .vert/.frag 不是构建依赖，修改后不触发重新构建也不重新复制，
#      调试 shader 时跑的是旧 shader（Docs/Mac渲染.md §4 明确警告过这点）。
#   3. Vulkan 需要 SPIR-V、Metal 需要 metallib，运行时读文本无法覆盖，
#      这也是旧 Vulkan 后端 m_shaderStages 永远为空的间接原因。
#
# 语言按扩展名推断：
#   .vert / .frag / .comp  -> glsl      （文本，末尾补 NUL，可直接交给 glShaderSource）
#   .metal                 -> msl       （文本，末尾补 NUL）
#   .spv                   -> spirv     （二进制，4 字节对齐的 SPIR-V 字流）
#   .metallib              -> metallib   （二进制）
#
# 入参（通过 -D 传入）：
#   EMBED_INPUT_LIST  以 '|' 分隔的输入文件绝对路径列表
#   EMBED_OUTPUT      输出 .cpp 的绝对路径
#
# 文本类 shader 支持一条极简的 `#include "name"` 指令（见 _embed_expand_includes）：
# RT 的全部 shader 共用同一个 std140 pushConstant 块，块的字段顺序必须与
# C++ 侧的 PushConstants 结构逐字节一致。std140 布局出错不会报编译错误，
# 只会算出错误的偏移，因此这份声明必须只有一处，不能在 9 个文件里各抄一份。
# ============================================================================

if(NOT DEFINED EMBED_INPUT_LIST)
    message(FATAL_ERROR "EmbedShaders: EMBED_INPUT_LIST is required")
endif()
if(NOT DEFINED EMBED_OUTPUT)
    message(FATAL_ERROR "EmbedShaders: EMBED_OUTPUT is required")
endif()

# 按扩展名判定语言与是否为文本
function(_embed_classify path out_language out_is_text)
    get_filename_component(_ext "${path}" EXT)
    string(TOLOWER "${_ext}" _ext)
    if(_ext STREQUAL ".vert" OR _ext STREQUAL ".frag" OR _ext STREQUAL ".comp")
        set(${out_language} "glsl" PARENT_SCOPE)
        set(${out_is_text} TRUE PARENT_SCOPE)
    elseif(_ext STREQUAL ".metal")
        set(${out_language} "msl" PARENT_SCOPE)
        set(${out_is_text} TRUE PARENT_SCOPE)
    elseif(_ext STREQUAL ".spv")
        set(${out_language} "spirv" PARENT_SCOPE)
        set(${out_is_text} FALSE PARENT_SCOPE)
    elseif(_ext STREQUAL ".metallib")
        set(${out_language} "metallib" PARENT_SCOPE)
        set(${out_is_text} FALSE PARENT_SCOPE)
    else()
        message(FATAL_ERROR "EmbedShaders: unsupported shader extension '${_ext}' for ${path}")
    endif()
endfunction()

set(_body "")
set(_table "")
set(_index 0)

# ----------------------------------------------------------------------------
# 展开文本 shader 中的 `#include "name"`
#
# 只支持「与被包含文件同目录、双引号、单行」这一种形式，故意不做搜索路径、
# 条件包含、宏——那属于重新实现 C 预处理器。当前唯一用途是共享
# pushConstant 块声明（rx_push_constants.glsl）。
#
# 注意：同一个文件被同一 shader 包含两次会被展开两次，从而在 GLSL 编译期
# 报重定义。这是刻意的——静默去重会掩盖 shader 里真实的书写错误。
# ----------------------------------------------------------------------------
function(_embed_expand_includes input_path out_text)
    file(READ "${input_path}" _text)
    get_filename_component(_dir "${input_path}" DIRECTORY)

    # 深度上限同时充当循环包含的保护：超限即报错，而不是无限展开
    set(_maxDepth 8)
    foreach(_pass RANGE 1 ${_maxDepth})
        string(REGEX MATCH "#include[ \t]*\"[^\"]+\"" _directive "${_text}")
        if(_directive STREQUAL "")
            break()
        endif()
        if(_pass EQUAL ${_maxDepth})
            message(FATAL_ERROR
                "EmbedShaders: ${input_path} 的 #include 嵌套超过 ${_maxDepth} 层（可能存在循环包含）")
        endif()

        string(REGEX REPLACE "#include[ \t]*\"([^\"]+)\"" "\\1" _name "${_directive}")
        set(_includePath "${_dir}/${_name}")
        if(NOT EXISTS "${_includePath}")
            message(FATAL_ERROR "EmbedShaders: ${input_path} 包含的文件不存在：${_name}")
        endif()
        file(READ "${_includePath}" _included)
        string(REPLACE "${_directive}" "${_included}" _text "${_text}")
    endforeach()

    set(${out_text} "${_text}" PARENT_SCOPE)
endfunction()

# EMBED_INPUT_LIST 以 '|' 分隔（见 CMakeLists.txt 中传参处的说明），还原成 CMake 列表。
# 同时兼容以分号传入的情况。
string(REPLACE "|" ";" _inputs "${EMBED_INPUT_LIST}")

# 展开后的文本先落到临时目录再按 HEX 读取：HEX 读取对文本与二进制统一，
# 避免 CMake 在 NUL / 非 UTF-8 字节上的处理差异。
get_filename_component(_outdir "${EMBED_OUTPUT}" DIRECTORY)
set(_expandDir "${_outdir}/expanded")
file(MAKE_DIRECTORY "${_expandDir}")

foreach(_input IN LISTS _inputs)
    if(NOT EXISTS "${_input}")
        message(FATAL_ERROR "EmbedShaders: input file not found: ${_input}")
    endif()

    get_filename_component(_name "${_input}" NAME)
    _embed_classify("${_input}" _language _is_text)

    set(_readFrom "${_input}")
    if(_is_text)
        _embed_expand_includes("${_input}" _expanded)
        set(_readFrom "${_expandDir}/${_name}")
        file(WRITE "${_readFrom}" "${_expanded}")
    endif()

    # HEX 读取对文本与二进制都安全，避免 CMake 对 NUL / 非 UTF-8 字节的处理差异
    file(READ "${_readFrom}" _hex HEX)
    string(LENGTH "${_hex}" _hexlen)
    math(EXPR _bytes "${_hexlen} / 2")

    # 一次性把 hex 串转成 "0xAB,0xCD,..."。
    # 不用 foreach 逐字节拼接：那样对 6KB 的 culling.comp 需要 6000 次
    # string(APPEND)，配置耗时会明显增加。
    string(REGEX REPLACE "(..)" "0x\\1," _literal "${_hex}")
    # 每 16 字节换行，避免生成单行数万字符（部分编译器与 diff 工具对超长行不友好）
    string(REGEX REPLACE "((0x[0-9a-fA-F][0-9a-fA-F],){16})" "\\1\n    " _literal "${_literal}")

    # 文本类语言补一个 NUL 结尾，使数据可直接当 const char* 使用；
    # size 字段仍为不含 NUL 的实际字节数，二进制语言不受影响。
    if(_is_text)
        string(APPEND _literal "0x00")
    endif()

    string(APPEND _body
        "// ${_name} (${_language}, ${_bytes} bytes)\n"
        "static const unsigned char kBlob${_index}[] = {\n    ${_literal}\n};\n\n")

    string(APPEND _table
        "    { \"${_name}\", \"${_language}\", kBlob${_index}, ${_bytes}u },\n")

    math(EXPR _index "${_index} + 1")
endforeach()

set(_generated
"// ==========================================================================
// 本文件由 CMake/EmbedShaders.cmake 自动生成，请勿手工修改。
// 修改 shader 请改 src/shader/*.vert|frag|comp，重新构建即会重新生成
// （shader 文件已声明为构建依赖）。
// ==========================================================================

namespace Render
{
    namespace shader
    {
        namespace generated
        {

            struct RawShaderBlob
            {
                const char* name;
                const char* language;
                const unsigned char* data;
                unsigned long long sizeBytes;
            };

${_body}            extern const RawShaderBlob kShaderBlobs[] = {
${_table}            };

            extern const unsigned long long kShaderBlobCount = ${_index}ull;

        }  // namespace generated
    }  // namespace shader
}  // namespace Render
")

# 仅在内容变化时写入，避免每次构建都触发下游重编译
if(EXISTS "${EMBED_OUTPUT}")
    file(READ "${EMBED_OUTPUT}" _existing)
    if(_existing STREQUAL "${_generated}")
        return()
    endif()
endif()

get_filename_component(_outdir "${EMBED_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_outdir}")
file(WRITE "${EMBED_OUTPUT}" "${_generated}")
message(STATUS "[EmbedShaders] generated ${EMBED_OUTPUT} (${_index} blobs)")
