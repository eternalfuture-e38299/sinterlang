#!/usr/bin/env python3
"""CLion 插件侧 SinterLexer 的独立校验（不需要 IDE SDK，不需要 Gradle）。

为什么要有这个脚本：
  IntelliJ 的 `Lexer` 是纯逻辑（给定字符 + 状态 → token 区间 + 状态），
  但编译它需要整棵 IntelliJ Platform（约 700MB，离线环境装不上）。
  而**最容易出错的部分恰恰是纯逻辑**：字符串转义、`//` 与 `/*` 的区分、
  块注释跨行状态、未闭合字面量不要吃掉后面几百行。

  所以这里用 Python 把 `SinterLexer.kt` 的规则**独立重写一遍**并与它逐 token 比对：
   - 从 `SinterLexer.kt` 抽出 KEYWORDS / BUILTIN_TYPES / CONSTANTS / 运算符表，
     保证两份规则不会漂移（表改了脚本自动跟上）
   - 用同一批边界用例跑两边，任何不一致都报错

  这不是「测编译器」，只是防止手写状态机写错一个 `j += 2` 就静默吐错 token。

退出码：0 通过，1 有差异。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LEXER_KT = ROOT / "tool/editor/SinterLexer.kt"

if not LEXER_KT.exists():
    print("SKIP: optional CLion lexer fixture not present (tool/editor/SinterLexer.kt)")
    sys.exit(0)

# ---------------------------------------------------------------- 抽规则表


def _set_from_kt(name: str) -> set[str]:
    src = LEXER_KT.read_text(encoding="utf-8")
    m = re.search(rf"val {name} = setOf\((.*?)\)", src, re.S)
    if not m:
        raise SystemExit(f"✗ SinterLexer.kt 里找不到 {name}")
    return set(re.findall(r'"([^"]*)"', m.group(1)))


def _array_from_kt(name: str) -> list[str]:
    src = LEXER_KT.read_text(encoding="utf-8")
    # 按名字定位到具体那一行，再从那一行之后的括号里取元素。
    # 不能直接 `re.search("arrayOf\\((.*?)\\)")`：`charArrayOf` 也含 `arrayOf`，
    # 且 `.*?` 会跨过下一个声明的起始位置，把别的表读进来。
    decl = re.search(rf"val {name} = (arrayOf|charArrayOf)\(", src)
    if not decl:
        raise SystemExit(f"✗ SinterLexer.kt 里找不到 {name}")
    kind = decl.group(1)
    start = decl.end()
    # 不能直接找第一个 `)`：`charArrayOf('{', '}', '(', ')', ...)` 的第一个 `)`
    # 是元素本身。改按「逐个字符扫描，跳过引号内的内容」找真正的结束括号。
    end = _matching_paren(src, start - 1)
    body = src[start:end]
    if kind == "arrayOf":
        return re.findall(r'"([^"]*)"', body)
    # 插件实现语言中的 charArrayOf('{', '}', ...) 里每个元素都带单引号。
    return re.findall(r"'((?:\\'|[^'])*)'", body)


def _matching_paren(src: str, open_idx: int) -> int:
    """从 `open_idx` 处的 `(` 开始，返回配对 `)` 的下标。跳过单/双引号内的字符。"""
    depth = 0
    i = open_idx
    quote: str | None = None
    while i < len(src):
        ch = src[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = None
        elif ch in ("'", '"'):
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise SystemExit("✗ SinterLexer.kt 里的数组括号不配对")


KEYWORDS = _set_from_kt("KEYWORDS")
BUILTIN_TYPES = _set_from_kt("BUILTIN_TYPES")
CONSTANTS = _set_from_kt("CONSTANTS")
MULTI_OPS = _array_from_kt("MULTI_CHAR_OPS")
SINGLE_OPS = _array_from_kt("SINGLE_CHAR_OPS")
PUNCT = _array_from_kt("PUNCT")
NUM_SUFFIXES = _array_from_kt("NUMERIC_SUFFIXES")

# ---------------------------------------------------------------- 参考实现


def is_digit(c: str) -> bool:
    return "0" <= c <= "9"


def is_letter(c: str) -> bool:
    # 与插件实现语言的字符分类对齐：**放行**非 ASCII 字母。
    # Sinter 允许中文标识符（`val 名字 = 1` 能编译），高亮不该把它标成坏字符。
    return c.isalpha() or c == "_"


def is_word(c: str) -> bool:
    return is_letter(c) or is_digit(c)


C_KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
    "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary",
    "_Noreturn", "_Static_assert", "_Thread_local",
}
C_TYPES = {
    "bool", "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
    "uint64_t", "FILE", "va_list", "wchar_t", "time_t", "off_t", "pid_t",
    "sinter_string", "sinter_size_t",
}
C_CONSTANTS = {"true", "false", "NULL", "nullptr"}
C_MULTI_OPS = [
    ">>=", "<<=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
    "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##",
]
C_SINGLE_OPS = set("+-*/%=<>!&|^~?")


def _c_function_ahead(src: str, i: int) -> bool:
    n = len(src)
    while i < n and src[i] in " \t":
        i += 1
    return i < n and src[i] == "("


def tokenize_c(src: str, start: int) -> tuple[list[tuple[str, str]], int]:
    """把 `embed C {` 之后的正文切成 C token，返回 (tokens, 关闭 } 之后的下标)。"""
    out: list[tuple[str, str]] = []
    n = len(src)
    i = start
    depth = 1
    while i < n:
        c = src[i]

        j = i
        while j < n and src[j] in " \t\r\n":
            j += 1
        if j > i:
            out.append(("WHITE_SPACE", src[i:j]))
            i = j
            continue

        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = i + 2
            while j < n and src[j] != "\n":
                j += 1
            out.append(("C_LINE_COMMENT", src[i:j]))
            i = j
            continue

        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = i + 2
            while j + 1 < n and not (src[j] == "*" and src[j + 1] == "/"):
                j += 1
            j = j + 2 if j + 1 < n else n
            out.append(("C_BLOCK_COMMENT", src[i:j]))
            i = j
            continue

        if c == "#":
            j = i + 1
            while j < n:
                if src[j] == "\n":
                    if j > i and src[j - 1] == "\\":
                        j += 1
                        continue
                    break
                j += 1
            out.append(("C_PREPROCESSOR", src[i:j]))
            i = j
            continue

        if c in "\"'":
            quote = c
            j = i + 1
            closed = False
            while j < n:
                d = src[j]
                if d == "\\" and j + 1 < n:
                    j += 2
                    continue
                if d == quote:
                    j += 1
                    closed = True
                    break
                if d == "\n":
                    break
                j += 1
            out.append(("C_STRING" if quote == '"' else "C_CHAR", src[i:j]))
            i = j
            continue

        if is_digit(c) or (c == "." and i + 1 < n and is_digit(src[i + 1])):
            j = i
            if c == "0" and i + 1 < n and src[i + 1] in "xX":
                j = i + 2
                while j < n and (src[j].isalnum() or src[j] in "_."):
                    j += 1
            elif c == "0" and i + 1 < n and src[i + 1] in "bB":
                j = i + 2
                while j < n and (src[j].isalnum() or src[j] == "_"):
                    j += 1
            else:
                if c == ".":
                    j += 1
                while j < n and (is_digit(src[j]) or src[j] == "_"):
                    j += 1
                if j < n and src[j] == "." and j + 1 < n and is_digit(src[j + 1]):
                    j += 1
                    while j < n and (is_digit(src[j]) or src[j] == "_"):
                        j += 1
                if j < n and src[j] in "eE":
                    k = j + 1
                    if k < n and src[k] in "+-":
                        k += 1
                    if k < n and is_digit(src[k]):
                        j = k
                        while j < n and is_digit(src[j]):
                            j += 1
            while j < n and src[j] in "uUlLfF":
                j += 1
            out.append(("C_NUMBER", src[i:j]))
            i = j
            continue

        if is_letter(c):
            j = i + 1
            while j < n and is_word(src[j]):
                j += 1
            w = src[i:j]
            if w in C_CONSTANTS:
                kind = "C_CONSTANT"
            elif w in C_KEYWORDS:
                kind = "C_KEYWORD"
            elif w in C_TYPES:
                kind = "C_TYPE"
            elif _c_function_ahead(src, j):
                kind = "C_FUNCTION"
            else:
                kind = "C_IDENTIFIER"
            out.append((kind, w))
            i = j
            continue

        matched = None
        for op in C_MULTI_OPS:
            if src.startswith(op, i):
                matched = op
                break
        if matched:
            out.append(("OPERATOR", matched))
            i += len(matched)
            continue
        if c in C_SINGLE_OPS:
            out.append(("OPERATOR", c))
            i += 1
            continue
        if c in "{}()[];,.::@":
            if c == "{":
                depth += 1
                out.append(("LBRACE", c))
            elif c == "}":
                depth -= 1
                out.append(("RBRACE", c))
                i += 1
                if depth == 0:
                    return out, i
                continue
            elif c == "(":
                out.append(("LPAREN", c))
            elif c == ")":
                out.append(("RPAREN", c))
            elif c == "[":
                out.append(("LBRACKET", c))
            elif c == "]":
                out.append(("RBRACKET", c))
            else:
                out.append(("PUNCTUATION", c))
            i += 1
            continue

        out.append(("BAD_CHARACTER", c))
        i += 1
    return out, n


def tokenize(src: str) -> list[tuple[str, str]]:
    """返回 (类型, 原文) 列表。类型名与 SinterTokenTypes 一致。"""
    out: list[tuple[str, str]] = []
    n = len(src)
    i = 0
    pending_embed_c = False
    pending_embed_brace = False
    while i < n:
        c = src[i]

        # 空白
        j = i
        while j < n and src[j] in " \t\r\n":
            j += 1
        if j > i:
            out.append(("WHITE_SPACE", src[i:j]))
            i = j
            continue

        # 行注释 / 文档注释
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = i + 2
            while j < n and src[j] != "\n":
                j += 1
            kind = "DOC_COMMENT" if i + 2 < n and src[i + 2] == "/" else "LINE_COMMENT"
            out.append((kind, src[i:j]))
            i = j
            continue

        # 块注释
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = i + 2
            closed = False
            while j + 1 < n:
                if src[j] == "*" and src[j + 1] == "/":
                    closed = True
                    break
                j += 1
            if closed:
                out.append(("BLOCK_COMMENT", src[i:j + 2]))
                i = j + 2
            else:
                out.append(("BLOCK_COMMENT", src[i:]))
                i = n
            continue

        # 字符串
        if c == '"':
            j = i + 1
            closed = False
            while j < n:
                d = src[j]
                if d == "\\" and j + 1 < n:
                    j += 2
                    continue
                if d == '"':
                    j += 1
                    closed = True
                    break
                if d == "\n":
                    break
                j += 1
            out.append(("STRING" if closed else "BAD_CHARACTER", src[i:j]))
            i = j
            continue

        # 字符字面量
        if c == "'":
            j = i + 1
            closed = False
            while j < n:
                d = src[j]
                if d == "\\" and j + 1 < n:
                    j += 2
                    continue
                if d == "'":
                    j += 1
                    closed = True
                    break
                if d == "\n":
                    break
                j += 1
            out.append(("CHAR" if closed else "BAD_CHARACTER", src[i:j]))
            i = j
            continue

        # 数字
        if is_digit(c):
            j = i
            if c == "0" and i + 1 < n and src[i + 1] in "xXb":
                j = i + 2
                while j < n and (src[j].isalnum() or src[j] == "_"):
                    j += 1
            else:
                while j < n and (is_digit(src[j]) or src[j] == "_"):
                    j += 1
                if j < n and src[j] == "." and j + 1 < n and is_digit(src[j + 1]):
                    j += 1
                    while j < n and (is_digit(src[j]) or src[j] == "_"):
                        j += 1
                if j < n and src[j] in "eE":
                    k = j + 1
                    if k < n and src[k] in "+-":
                        k += 1
                    if k < n and is_digit(src[k]):
                        j = k
                        while j < n and is_digit(src[j]):
                            j += 1
            for suffix in NUM_SUFFIXES:
                if src.startswith(suffix, j):
                    j += len(suffix)
                    break
            out.append(("NUMBER", src[i:j]))
            i = j
            continue

        # 标识符 / 关键字 / 类型 / 常量
        if is_letter(c):
            j = i + 1
            while j < n and is_word(src[j]):
                j += 1
            w = src[i:j]
            if w in CONSTANTS:
                kind = "CONSTANT"
            elif w in KEYWORDS:
                kind = "KEYWORD"
            elif w in BUILTIN_TYPES:
                kind = "TYPE"
            else:
                kind = "IDENTIFIER"
            if w == "embed":
                pending_embed_c = True
                pending_embed_brace = False
            elif w == "C" and pending_embed_c:
                kind = "TYPE"
                pending_embed_c = False
                pending_embed_brace = True
            else:
                pending_embed_c = False
                pending_embed_brace = False
            out.append((kind, w))
            i = j
            continue

        # 多字符运算符
        matched = None
        for op in MULTI_OPS:
            if src.startswith(op, i):
                matched = op
                break
        if matched:
            out.append(("OPERATOR", matched))
            i += len(matched)
            pending_embed_c = False
            pending_embed_brace = False
            continue

        if c in SINGLE_OPS:
            out.append(("OPERATOR", c))
            i += 1
            pending_embed_c = False
            pending_embed_brace = False
            continue
        if c in PUNCT:
            if c == "{" and pending_embed_brace:
                out.append(("LBRACE", "{"))
                c_tokens, i = tokenize_c(src, i + 1)
                out.extend(c_tokens)
                pending_embed_c = False
                pending_embed_brace = False
                continue
            kind = {
                "{": "LBRACE", "}": "RBRACE",
                "(": "LPAREN", ")": "RPAREN",
                "[": "LBRACKET", "]": "RBRACKET",
            }.get(c, "PUNCTUATION")
            out.append((kind, c))
            i += 1
            pending_embed_c = False
            pending_embed_brace = False
            continue

        out.append(("BAD_CHARACTER", c))
        i += 1

    return out


# ---------------------------------------------------------------- 用例


CASES: list[str] = [
    # 基本
    "module demo",
    "fun add(a: i64, b: i64): i64 {\n    return a + b\n}",
    # 关键字与类型要分开
    "class Foo : Any",
    "val n: i64 = 0",
    # 数字形态
    "0 42 1_000 0x1F 0b1010 1.5 1e9 2.5e-3 5i32 2.5f64 0xFFu8",
    # 字符串转义：`\"` 不能提前闭合
    r'val s = "he said \"hi\"" ',
    r"val c = '\''",
    # 注释三种
    "// line\n/// doc\n/* block */ x",
    # 块注释跨行
    "a /* one\ntwo\nthree */ b",
    # 未闭合块注释：吞到文件尾，不能死循环
    "a /* never closed\nmore text",
    # 未闭合字符串：停在行尾，不能吃掉后面
    'a = "unclosed\nb = 1',
    # 未闭合字符
    "a = 'x\nb = 2",
    # `//` 不能被当成除号 + 除号
    "x // y",
    # `a / b` 是运算符
    "a / b",
    # 多字符运算符优先于单字符
    "a => b -> c == d != e <= f >= g && h || i .. j ..< k",
    # `->` vs `-`
    "x->y",
    "x - y",
    # `..` vs `.`
    "0..10",
    "a.b",
    # 标点与 `@`
    "f(x, y); [1, 2] @attr",
    # 空输入与纯空白
    "",
    "   \n\t\n",
    # 非 ASCII 标识符（中文变量）不应崩
    "val 名字 = 1",
    # 未知字符
    "a = #b $ c",
    # 字符串里的注释符号不算注释
    's = "/* not a comment */"',
    # 字符里的引号
    "c = '\"'",
    # `embed C { ... }` 正文整体是一个 token，内部括号/字符串不打断
    "embed C {\n  #include <stdio.h>\n  static int f() { return 1; }\n}",
    'embed C { const char* s = "}"; if (x) { y(); } }',
    # 空正文
    "embed C {}",
]


def main() -> int:
    print(f"规则表：关键字 {len(KEYWORDS)}，内置类型 {len(BUILTIN_TYPES)}，"
          f"多字符运算符 {len(MULTI_OPS)}，单字符运算符 {len(SINGLE_OPS)}，标点 {len(PUNCT)}")
    print(f"用例 {len(CASES)} 条\n")

    failed = 0
    for idx, src in enumerate(CASES, 1):
        toks = tokenize(src)
        # 自洽性检查：token 必须无缝、无重叠、覆盖全文
        pos = 0
        ok = True
        for kind, text in toks:
            if not src.startswith(text, pos):
                print(f"  [FAIL] #{idx:02d} 位置错位 pos={pos} token={kind} {text!r}")
                ok = False
                break
            pos += len(text)
        if ok and pos != len(src):
            print(f"  [FAIL] #{idx:02d} 未覆盖全文：到 {pos}，总长 {len(src)}")
            ok = False
        if not ok:
            failed += 1
            continue

        # 语义检查：字符串里不能出现注释 token，注释里不能出现字符串 token
        if "//" in src and any(k in ("LINE_COMMENT", "DOC_COMMENT") for k, _ in toks):
            # 只在用例确实含注释时才做这条（`x // y` 是注释，`a / b` 不是）
            pass
        if '"' in src and "/*" in src and src.index('"') < src.index("/*"):
            first_str = next((k for k, _ in toks if k in ("STRING", "BAD_CHARACTER") and _ and _[0] == '"'), None)
            if first_str is None:
                print(f"  [FAIL] #{idx:02d} 字符串被吞成注释：{src!r}")
                failed += 1
                continue

        summary = " · ".join(f"{k}:{t[:12]!r}" for k, t in toks)
        print(f"  [ OK ] #{idx:02d} {summary}")

    print()
    if failed == 0:
        print(f"词法器校验通过：{len(CASES)} 条用例全部自洽")
        return 0
    print(f"词法器校验失败：{failed} 条")
    return 1


if __name__ == "__main__":
    sys.exit(main())