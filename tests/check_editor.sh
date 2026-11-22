#!/bin/sh
# tests/check_editor.sh —— 编辑器侧（LSP + CLion 插件）验收
#
#   sh tests/check_editor.sh [sinter-lsp]
#
# 与 tests/check.sh 分开：那边判定的是**编译器**（生成的 C 是否能跑），
# 这边判定的是**编辑器体验**（补全 / 格式化 / 模板 / 高亮是否如约）。
# 两者改动风险不同、跑起来耗时不同，混在一起会让编译器回归变慢。
#
# 五道关卡：
#   1. sinter-lsp --self-test      语言服务器内置断言（JSON、协议、补全、格式化、模板）
#   2. tool/test_lsp_format.py     真 stdio 协议：起进程、发帧、验 capabilities 与格式化文本
#   3. tests/check_snippets.py   LSP snippet 与 CLion Live Template 正文一致性
#   4. tests/check_lexer.py      CLion 插件侧词法器规则自洽（不需要 IDE SDK）
#   5. tests/check_plugin.py     插件源码结构 + 新建工程模板完整性
#
# 注意：/sdcard、/storage 是 noexec 挂载点，二进制要先复制到 /tmp 再执行。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LSP=${1:-${LSP:-$ROOT/src/sinter-lsp/build/cmake/sinter-lsp}}
WORK=${TMPDIR:-/tmp}/sinter_editor_check_$$

pass=0
fail=0
note() { printf '%s %s\n' "$1" "$2"; }

cleanup() { rm -f "$WORK.lsp"; }
trap cleanup EXIT

# ---- 准备可执行副本 ----
if [ ! -f "$LSP" ]; then
    note "FAIL" "找不到 sinter-lsp（$LSP），先跑 sh tests/check_sinter_lsp.sh"
    exit 1
fi
cp "$LSP" "$WORK.lsp" && chmod 755 "$WORK.lsp" || {
    note "FAIL" "无法复制 sinter-lsp 到 $WORK.lsp"
    exit 1
}

# ---- 1. 内置自测 ----
echo "== sinter-lsp --self-test =="
if out=$("$WORK.lsp" --self-test 2>&1); then
    echo "$out" | tail -3
    pass=$((pass + 1))
else
    echo "$out" | grep -E '\[FAIL\]|failed' | head -20
    note "FAIL" "self-test"
    fail=$((fail + 1))
fi

# ---- 2. 真协议测试 ----
echo
echo "== LSP 协议（tool/test_lsp_format.py） =="
if command -v python3 >/dev/null 2>&1; then
    if SINTER_LSP="$WORK.lsp" python3 "$ROOT/tool/test_lsp_format.py" >"$WORK.proto.txt" 2>&1; then
        grep -E '^\s+(缩进|运算符|`|else|字符串)|snippets=' "$WORK.proto.txt" | head -12
        pass=$((pass + 1))
    else
        tail -25 "$WORK.proto.txt"
        note "FAIL" "LSP 协议测试"
        fail=$((fail + 1))
    fi
    rm -f "$WORK.proto.txt"
else
    note "SKIP" "没有 python3，跳过协议测试"
fi

# ---- 3. 模板一致性 ----
echo
echo "== 模板一致性（tests/check_snippets.py） =="
if command -v python3 >/dev/null 2>&1; then
    if out=$(python3 "$ROOT/tests/check_snippets.py" 2>&1); then
        echo "$out" | tail -2
        pass=$((pass + 1))
    else
        echo "$out" | grep -Ei 'FAIL|不一致|✗' | head -20
        note "FAIL" "模板一致性"
        fail=$((fail + 1))
    fi
else
    note "SKIP" "没有 python3，跳过模板一致性"
fi

# ---- 4. 词法器规则 ----
echo
echo "== 插件词法器（tests/check_lexer.py） =="
if command -v python3 >/dev/null 2>&1; then
    if out=$(python3 "$ROOT/tests/check_lexer.py" 2>&1); then
        echo "$out" | tail -2
        pass=$((pass + 1))
    else
        echo "$out" | grep -E 'FAIL|✗|失败' | head -20
        note "FAIL" "插件词法器"
        fail=$((fail + 1))
    fi
else
    note "SKIP" "没有 python3，跳过词法器校验"
fi

# ---- 5. 插件源码结构 ----
echo
echo "== 插件源码结构（tests/check_plugin.py） =="
if command -v python3 >/dev/null 2>&1; then
    if out=$(python3 "$ROOT/tests/check_plugin.py" 2>&1); then
        echo "$out" | tail -1
        pass=$((pass + 1))
    else
        echo "$out" | grep -E 'FAIL|失败' | head -20
        note "FAIL" "插件源码结构"
        fail=$((fail + 1))
    fi
else
    note "SKIP" "没有 python3，跳过插件结构检查"
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]