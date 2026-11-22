#!/usr/bin/env python3
"""CLion 插件源码的轻量结构检查（不需要 IDE SDK / Gradle）。

为什么需要它：
  编译插件要整棵 IntelliJ Platform（约 1GB），离线环境装不上。
  但有一类错误跟 SDK 无关、又最容易在手工编辑后出现：
    - 括号 / 大括号不配对
    - 字符串字面量未闭合（会把后面几十行吞成字符串）
    - 文件里引用了`SinterXxx` 但那个类根本不存在 / 名字拼错
    - plugin.xml 里 implementation 指向的类在源码里找不到

  本脚本只做这些「文本层」检查，不做类型检查。它不能替代 `gradle buildPlugin`，
  但能在没有网络的机器上抓出装配错误。

退出码：0 通过，1 有问题。
"""
from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "tool/editor/clion-plugin"
KT_DIR = PLUGIN / "src/main/kotlin/dev/sinter/clion"
XML = PLUGIN / "src/main/resources/META-INF/plugin.xml"
GRADLE = PLUGIN / "build.gradle.kts"

if not PLUGIN.exists():
    print("SKIP: optional CLion plugin project not present (tool/editor/clion-plugin/)")
    sys.exit(0)


def strip_comments_and_strings(src: str) -> str:
    """把注释与字符串内容替换成空格，只留结构字符。

    保留引号本身，这样三方引号块的边界仍然可判断。
    """
    out: list[str] = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        # 行注释
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
            continue
        # 块注释
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            out.append(" ")
            out.append(" ")
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            if i + 1 < n:
                out.append(" ")
                out.append(" ")
                i += 2
            continue
        # 三引号字符串
        if src.startswith('"""', i):
            out.append('"""')
            i += 3
            while i < n and not src.startswith('"""', i):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append('"""')
                i += 3
            continue
        # 普通字符串
        if c == '"':
            out.append('"')
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append(" ")
                    out.append(" ")
                    i += 2
                    continue
                if src[i] == '"':
                    out.append('"')
                    i += 1
                    break
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            continue
        # 字符字面量
        if c == "'":
            out.append("'")
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append(" ")
                    out.append(" ")
                    i += 2
                    continue
                if src[i] == "'":
                    out.append("'")
                    i += 1
                    break
                out.append(" ")
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def check_balance(path: Path, src: str) -> list[str]:
    stripped = strip_comments_and_strings(src)
    problems = []
    pairs = {")": "(", "]": "[", "}": "{"}
    stack: list[tuple[str, int]] = []
    line = 1
    for ch in stripped:
        if ch == "\n":
            line += 1
        elif ch in "([{":
            stack.append((ch, line))
        elif ch in ")]}":
            if not stack:
                problems.append(f"{path.name}:{line} 多余的 `{ch}`")
            elif stack[-1][0] != pairs[ch]:
                op, ol = stack[-1]
                problems.append(f"{path.name}:{line} `{ch}` 与第 {ol} 行的 `{op}` 不匹配")
                stack.pop()
            else:
                stack.pop()
    for op, ol in stack:
        problems.append(f"{path.name}:{ol} `{op}` 没有闭合")
    return problems


def check_string_closure(path: Path, src: str) -> list[str]:
    """未闭合的字符串会让后续整段代码失效，是最隐蔽的错误之一。"""
    problems = []
    i, n, line = 0, len(src), 1
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                if src[i] == "\n":
                    line += 1
                i += 1
            i += 2
            continue
        if src.startswith('"""', i):
            start_line = line
            i += 3
            while i < n and not src.startswith('"""', i):
                if src[i] == "\n":
                    line += 1
                i += 1
            if i >= n:
                problems.append(f"{path.name}:{start_line} 三引号字符串未闭合")
                break
            i += 3
            continue
        if c == '"':
            start_line = line
            i += 1
            closed = False
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if src[i] == '"':
                    i += 1
                    closed = True
                    break
                if src[i] == "\n":
                    break
                i += 1
            if not closed:
                problems.append(f"{path.name}:{start_line} 字符串字面量未闭合")
            continue
        # 插件实现语言的单引号只能是字符字面量（`)`、'\\'、'\n'），不是字符串引号。
        # 不跳过它，`c == '"'` 里的 `"` 就会被当成字符串起点，然后一路扫到文件尾。
        if c == "'":
            start_line = line
            i += 1
            closed = False
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if src[i] == "'":
                    i += 1
                    closed = True
                    break
                if src[i] == "\n":
                    break
                i += 1
            if not closed:
                problems.append(f"{path.name}:{start_line} 字符字面量未闭合")
            continue
        i += 1
    return problems


def declared_types(src: str) -> set[str]:
    """找出源码里声明过的顶层类型名（class / object / interface）。"""
    stripped = strip_comments_and_strings(src)
    return set(re.findall(
        r"\b(?:class|object|interface)\s+([A-Z][A-Za-z0-9_]*)", stripped))


def main() -> int:
    if not KT_DIR.is_dir():
        print(f"✗ 找不到源码目录 {KT_DIR}")
        return 1

    kt_files = sorted(KT_DIR.glob("*.kt"))
    if not kt_files:
        print("✗ 没有 .kt 文件")
        return 1

    problems: list[str] = []
    all_types: set[str] = set()

    for f in kt_files:
        src = f.read_text(encoding="utf-8")
        problems += check_balance(f, src)
        problems += check_string_closure(f, src)
        all_types |= declared_types(src)

    # ---- 未解析的 SinterXxx 引用 ----
    # 只查自己包名前缀的类名：IDE SDK 的类不在这里判定。
    for f in kt_files:
        src = f.read_text(encoding="utf-8")
        stripped = strip_comments_and_strings(src)
        for name in set(re.findall(r"\b(Sinter[A-Z][A-Za-z0-9_]*)\b", stripped)):
            if name not in all_types:
                problems.append(f"{f.name} 引用了不存在的类型 `{name}`")

    # ---- plugin.xml 的 implementation 必须真实存在 ----
    xml_classes: set[str] = set()
    if XML.is_file():
        tree = ET.parse(XML)
        for el in tree.getroot().iter():
            for attr in ("implementation", "implementationClass", "serviceImplementation",
                         "instance", "class"):
                v = el.get(attr)
                if v and v.startswith("dev.sinter.clion."):
                    xml_classes.add(v.rsplit(".", 1)[-1])
        for cls in sorted(xml_classes):
            if cls not in all_types:
                problems.append(f"plugin.xml 引用了不存在的类 `{cls}`")
    else:
        problems.append("找不到 plugin.xml")

    # ---- 新建工程模板必须真的存在 ----
    template_kt = KT_DIR / "SinterProjectTemplate.kt"
    if template_kt.is_file():
        src = template_kt.read_text(encoding="utf-8")
        ids = re.findall(r'id\s*=\s*"([^"]+)"', src)
        base = PLUGIN / "src/main/resources/projectTemplates"
        for tid in sorted(set(ids)):
            for rel in ("sinter.toml", "src/main.sinter"):
                if not (base / tid / rel).is_file():
                    problems.append(f"模板 `{tid}` 缺少 resources/projectTemplates/{tid}/{rel}")
    else:
        problems.append("找不到 SinterProjectTemplate.kt")

    # ---- Live Template XML 必须能被解析 ----
    tmpl = PLUGIN / "src/main/resources/liveTemplates/Sinter.xml"
    if tmpl.is_file():
        try:
            ET.parse(tmpl)
        except ET.ParseError as e:
            problems.append(f"liveTemplates/Sinter.xml 解析失败：{e}")
    else:
        problems.append("找不到 liveTemplates/Sinter.xml")

    # ---- build.gradle.kts 关键项 ----
    if GRADLE.is_file():
        g = GRADLE.read_text(encoding="utf-8")
        for need in ("org.jetbrains.intellij", "type.set(\"CL\")", "jvmToolchain"):
            if need not in g:
                problems.append(f"build.gradle.kts 缺少 `{need}`")
    else:
        problems.append("找不到 build.gradle.kts")

    print(f"插件源文件 {len(kt_files)} 个，声明类型 {len(all_types)} 个，"
          f"plugin.xml 引用类 {len(xml_classes)} 个")
    for f in kt_files:
        print(f"  · {f.name}")
    print()

    if not problems:
        print("插件源码结构检查通过")
        return 0

    for p in problems:
        print(f"  [FAIL] {p}")
    print(f"\n插件源码结构检查失败：{len(problems)} 处")
    return 1


if __name__ == "__main__":
    sys.exit(main())