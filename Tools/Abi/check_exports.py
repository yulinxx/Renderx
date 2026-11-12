#!/usr/bin/env python3
"""Renderx 导出符号基线比对。

用途：快照 libRenderX 的 C ABI 导出符号，并在后续构建上做增量比对。
规则：**只允许新增**。删除或改名一律视为破坏性变更，脚本以非零码退出。

为什么需要：Renderx 的公开面是 include/render/renderx.h 里的 rx* C ABI，
下游（RenderBridge / 宿主 DLL）在链接期才会发现符号缺失或改名。等链接
报错时往往已经跨了几个模块，很难判断是谁删了导出。把符号集存成基线后，
任何删除都会在比对这一步被点名。

局限：C 函数的**参数/返回值变化**不会改变符号名，nm 看不出来。这类改动
必须配合 renderx.h 的审查与 ABI 版本号（rxGetAbiVersion）一起判断，本脚本
只能保证「符号集合不缩水」。

用法：
    python3 Tools/Abi/check_exports.py --library <libRenderX 路径>
    python3 Tools/Abi/check_exports.py --update --library <libRenderX 路径>

不传 --library 时按 Renderx/build*/bin_Qt6/{Release,Debug}/libRenderX.* 等
常见布局自动探测，取修改时间最新的一个。

符号归一化：Mach-O 的 C 符号带一个前导下划线（_rxRuntimeCreate），ELF 不带
（rxRuntimeCreate）。基线统一剥离前导下划线，保证同一份文件跨平台可比。
"""

import argparse
import glob
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RENDERX_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
DEFAULT_BASELINE = os.path.join(SCRIPT_DIR, "exported-symbols.baseline.txt")

# 自动探测构建产物时尝试的布局（Renderx 独立构建与根工程构建两种）
LIBRARY_PATTERNS = [
    "build*/bin_Qt6/Release/libRenderX.dylib",
    "build*/bin_Qt6/Debug/libRenderX.dylib",
    "build*/bin_Qt6/Release/libRenderX.so",
    "build*/bin_Qt6/Debug/libRenderX.so",
    "build*/libRenderX.dylib",
    "build*/libRenderX.so",
]


def find_library():
    """按常见布局探测 libRenderX，取最新修改的一个。"""
    candidates = []
    for pattern in LIBRARY_PATTERNS:
        candidates.extend(glob.glob(os.path.join(RENDERX_ROOT, pattern)))
    if not candidates:
        return None
    return max(candidates, key=os.path.getmtime)


def read_symbols(library):
    """用 nm 读取全局已定义符号，归一化为不带前导下划线的名字集合。"""
    if not os.path.isfile(library):
        sys.exit(f"[abi] library not found: {library}")

    if sys.platform == "darwin":
        # -g 只看外部符号，-U 排除未定义符号
        cmd = ["nm", "-gU", library]
    else:
        cmd = ["nm", "-D", "--defined-only", library]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"[abi] nm failed ({result.returncode}): {result.stderr.strip()}")

    symbols = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        # 格式：<地址> <类型> <名字>；无地址的行（如归档成员头）跳过
        if len(fields) < 3:
            continue
        name = fields[-1]
        # Mach-O 的 C 符号统一带一个前导下划线，剥掉后与 ELF 对齐
        if name.startswith("_"):
            name = name[1:]
        if name:
            symbols.add(name)
    return symbols


def load_baseline(path):
    """读基线文件：跳过注释与空行。"""
    if not os.path.isfile(path):
        sys.exit(f"[abi] baseline not found: {path}\n[abi] create it with --update")
    symbols = set()
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            symbols.add(line)
    return symbols


def write_baseline(path, symbols, library):
    """写基线文件：固定的头注释 + 排序后的符号。"""
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("# Renderx C ABI 导出符号基线（nm 全局已定义符号，已剥离 Mach-O 前导下划线）\n")
        # 只记文件名，避免把生成者的本机绝对路径写进仓库
        handle.write(f"# 来源：{os.path.basename(library)}\n")
        handle.write("# 更新：python3 Tools/Abi/check_exports.py --update --library <libRenderX>\n")
        handle.write("# 规则：只允许新增；删除或改名视为破坏，必须先确认再显式更新本文件。\n")
        handle.write("# 注意：C 函数签名变化不会改变符号名，本文件查不出来，需配合 renderx.h 与 ABI 版本审查。\n")
        for name in sorted(symbols):
            handle.write(f"{name}\n")


def main():
    parser = argparse.ArgumentParser(description="Compare libRenderX exported symbols against a baseline.")
    parser.add_argument("-l", "--library", help="path to libRenderX (auto-detected when omitted)")
    parser.add_argument("-b", "--baseline", default=DEFAULT_BASELINE, help="baseline file path")
    parser.add_argument("-u", "--update", action="store_true", help="rewrite the baseline from the library")
    args = parser.parse_args()

    library = args.library or find_library()
    if not library:
        sys.exit("[abi] cannot locate libRenderX; pass --library explicitly")

    current = read_symbols(library)

    if args.update:
        write_baseline(args.baseline, current, library)
        print(f"[abi] baseline written: {args.baseline} ({len(current)} symbols)")
        return 0

    baseline = load_baseline(args.baseline)
    removed = sorted(baseline - current)
    added = sorted(current - baseline)

    for name in added:
        print(f"[abi] added (allowed): {name}")

    if removed:
        for name in removed:
            print(f"[abi] REMOVED: {name}")
        print(f"[abi] FAIL: {len(removed)} exported symbol(s) disappeared from {library}")
        return 1

    print(f"[abi] OK: {len(baseline)} baseline symbols present, {len(added)} added")
    return 0


if __name__ == "__main__":
    sys.exit(main())
