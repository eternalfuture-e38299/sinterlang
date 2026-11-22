#!/usr/bin/env python3
"""校验 LSP snippet 与 CLion Live Template 的正文是否一致。

两边是同一套模板的两个副本：
  - tool/editor/features.cpp（kSnippets，LSP 的 insertText），可选夹具
  - tool/editor/Sinter.xml（Ctrl+J），可选夹具

为什么必须校验：模板写错不会报错，只会让用户拿到一段编译不过的代码，
而这类错误靠人工 review 极容易漏。这个脚本把差异摊平：

  1. 取 kSnippets 的每条 label 与 body
  2. 把 `${1:name}` / `${2}` 这类 LSP tabstop 归一成 `$VAR$` / `$VAR$`
  3. 取 XML 里每条 template 的 name 与 value（反转义 `&#10;`）
  4. 按「占位符序列 + 去掉占位符后的骨架」比较

名称并不需要一一相同（LSP 里 `fun` 在 Ctrl+J 里得叫 `pubfun` 才不跟关键字冲突），
所以校验按**顺序**配对：两张表的第 i 条必须结构等价。数量不同也会报错。

退出码：0 一致，1 不一致（打印具体差异）。
"""
from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FEATURES = ROOT / "tool/editor/features.cpp"
TEMPLATES = ROOT / "tool/editor/Sinter.xml"

if not FEATURES.exists() or not TEMPLATES.exists():
    print("SKIP: optional editor fixtures not present (tool/editor/)")
    sys.exit(0)

# LSP: ${1:name}  ${2}  $0
LSP_TABSTOP = re.compile(r"\$\{\d+:([^}]*)\}|\$\{\d+\}|\$0")
# Live Template: $VAR$
LT_VAR = re.compile(r"\$([A-Za-z_][A-Za-z0-9_]*)\$")
SKELETON_MARK = "\x00"


def lsp_snippets() -> list[tuple[str, str]]:
    src = FEATURES.read_text(encoding="utf-8")
    start = src.index("const Snippet kSnippets[] = {")
    end = src.index("};", start)
    body = src[start:end]

    out: list[tuple[str, str]] = []
    # 形如 {"module",   "模块头",           "module ${1:name}\n\n"},
    for m in re.finditer(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', body):
        label = m.group(1)
        text = m.group(3)
        text = text.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"').replace("\\\\", "\\")
        out.append((label, text))
    return out


def normalize(text: str) -> str:
    """把两种占位符语法都换成同一个标记，得到可比较的骨架。"""
    text = LSP_TABSTOP.sub(SKELETON_MARK, text)
    text = LT_VAR.sub(SKELETON_MARK, text)
    return text


def xml_templates() -> list[tuple[str, str]]:
    tree = ET.parse(TEMPLATES)
    out: list[tuple[str, str]] = []
    for tpl in tree.getroot().findall("template"):
        name = tpl.get("name") or ""
        value = tpl.get("value") or ""
        out.append((name, value))
    return out


def describe(a: str, b: str) -> str:
    al, bl = a.split("\n"), b.split("\n")
    lines = []
    for i in range(max(len(al), len(bl))):
        x = al[i] if i < len(al) else "<缺行>"
        y = bl[i] if i < len(bl) else "<缺行>"
        mark = "  " if x == y else "!!"
        if x != y:
            lines.append(f"    {mark} LSP: {x!r}")
            lines.append(f"    {mark} XML: {y!r}")
    return "\n".join(lines) if lines else f"    LSP: {a!r}\n    XML: {b!r}"


def main() -> int:
    snippets = lsp_snippets()
    templates = xml_templates()

    if not snippets:
        print("✗ 没能从 features.cpp 解析出任何 snippet，检查正则或表格式是否变了")
        return 1

    print(f"LSP snippet  {len(snippets)} 条")
    print(f"Live Template {len(templates)} 条")

    failed = 0
    if len(snippets) != len(templates):
        print(f"✗ 数量不一致：LSP {len(snippets)} vs XML {len(templates)}")
        failed += 1

    for i, ((label, lsp_text), (tpl_name, tpl_value)) in enumerate(zip(snippets, templates)):
        a = normalize(lsp_text).rstrip()
        b = normalize(tpl_value).rstrip()
        if a == b:
            print(f"  [ OK ] #{i + 1:02d} {label} ↔ {tpl_name}")
        else:
            failed += 1
            print(f"  [FAIL] #{i + 1:02d} {label} ↔ {tpl_name}")
            print(describe(a, b))

    print()
    if failed == 0:
        print(f"模板一致：{len(snippets)} 条全部对齐")
        return 0
    print(f"模板不一致：{failed} 处问题")
    return 1


if __name__ == "__main__":
    sys.exit(main())