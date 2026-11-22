#!/bin/sh
# tests/check.sh —— 回归验收脚本
#
#   sh tests/check.sh [sinter] [sinterc]
#
# 判定方式由目录里的文件决定，脚本里没有用例清单：
#   examples/hello        正向：语言基础面（类、RAII、字符串、管道、when…）
#   examples/collections  综合：泛型容器 + 多模块 + 标准库
#   examples/broken       反向：必须编译失败，且诊断命中 expect_error.txt
#   tests/cases/*         专题回归（A.1 的临时字符串实参在这里）
#
#   expect.txt        构建成功后 `sinter build --run` 的输出要与它逐行一致
#   expect_error.txt  必须构建失败，且每一行都要出现在诊断里
#   assert_gen.txt    生成 C 的结构断言，每行 `<运算符>|<子串>`，
#                     运算符为 <=N / >=N / ==N；`#` 开头的行是注释。
#                     计数对象是该工程 build 目录下 gen/ 里的全部 .c。
#
# 注意：/sdcard、/storage 是 noexec 挂载点，二进制要先复制到 /tmp 再执行。
set -u

SINTER=${1:-${SINTER:-/tmp/cbin/sinter}}
SINTERC=${2:-${SINTERC:-/tmp/cbin/sinterc}}
# 相对的编译器路径可能在子目录里执行时失效；先转绝对路径。
case "$SINTER" in /*) ;; *) SINTER="$PWD/$SINTER" ;; esac
case "$SINTERC" in /*) ;; *) SINTERC="$PWD/$SINTERC" ;; esac
# SINTER_ABS_NORMALIZED
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SINTER_VERSION=$(cat "$ROOT/VERSION")
export SINTER_VERSION
WORK=${TMPDIR:-/tmp}/sinter_projects_$$

for bin in "$SINTER" "$SINTERC"; do
    if [ -f "$bin" ] && [ ! -x "$bin" ]; then
        cp "$bin" "$WORK.$(basename "$bin")" && chmod 755 "$WORK.$(basename "$bin")"
    fi
done
[ -x "$SINTER" ] || { echo "错误: sinter 不可执行（$SINTER）"; exit 1; }
[ -x "$SINTERC" ] || { echo "错误: sinterc 不可执行（$SINTERC）"; exit 1; }

mkdir -p "$WORK"
# 用例被复制到 $WORK 时保留它相对 ROOT 的层级，path: 依赖才仍能解析。
ln -s "$ROOT/packages" "$WORK/packages"
export SINTERC
PATH="$(dirname "$SINTER"):$PATH"
export PATH

pass=0
fail=0
note() { printf '%s %s\n' "$1" "$2"; }

# 生成 C 的 C 标准关口：$(1)=工程目录
#
# 只认 `error:`（-pedantic-errors 把扩展用法升级成 error），不看 warning——
# std 里有一批已知的 unused parameter，它们不是「生成的 C 不合法」。
# 用 -fsyntax-only：只要语法与语义在严格 ISO C 下成立，不需要真的链接。
check_pedantic() {
    command -v gcc >/dev/null 2>&1 || return 0
    bad=""
    find "$1/build" -type f -name '*.c' -path '*gen*' 2>/dev/null | sort > "$1/ped_list.txt"
    while IFS= read -r f; do
        out=$(gcc -std=c17 -pedantic-errors -fsyntax-only "$f" -I "$(dirname "$f")" 2>&1 |
              grep -F 'error:' | head -3)
        [ -z "$out" ] || bad="$bad
  $out"
    done < "$1/ped_list.txt"
    [ -z "$bad" ] && return 0
    printf '%s\n' "$bad"
    return 1
}

# 生成 C 的结构断言：$(1)=工程目录（copy 后的工作目录） $(2)=assert_gen.txt 路径
check_gen() {
    gen="$1/gen_all.c"
    : > "$gen"
    find "$1/build" -type f -name '*.c' -path '*gen*' 2>/dev/null | sort > "$1/gen_list.txt"
    while IFS= read -r f; do
        cat "$f" >> "$gen"
    done < "$1/gen_list.txt"
    # 生成的 CMakeLists.txt 也进语料（`sinter build` 把它放在 build/cmake/，
    # `sinter emit-cmake` 放在 build/），这样「[build] cstd 有没有真的生效」
    # 也能用同一套断言表达，不必另开一种文件。
    find "$1/build" -type f -name 'CMakeLists.txt' 2>/dev/null | sort > "$1/cfg_list.txt"
    while IFS= read -r f; do
        cat "$f" >> "$gen"
    done < "$1/cfg_list.txt"
    bad=""
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac
        op=${line%%|*}
        pat=${line#*|}
        want=${op#??}
        n=$(grep -oF -- "$pat" "$gen" 2>/dev/null | wc -l | tr -d ' ')
        ok=1
        case "$op" in
            '<='*) [ "$n" -le "$want" ] || ok=0 ;;
            '>='*) [ "$n" -ge "$want" ] || ok=0 ;;
            '=='*) [ "$n" -eq "$want" ] || ok=0 ;;
            *) ok=0 ;;
        esac
        [ "$ok" -eq 1 ] || bad="$bad [$op 期望$want 实得$n :: $pat]"
    done < "$2"
    [ -z "$bad" ] && return 0
    printf '%s\n' "$bad"
    return 1
}

# $(1)=用例名 $(2)=源目录
check_one() {
    cname=$1
    src=$2
    [ -d "$src" ] || { note "FAIL" "$cname: 缺少工程目录"; fail=$((fail + 1)); return; }
    case "$src" in
        "$ROOT"/*) rel=${src#"$ROOT/"} ;;
        *) rel=$cname ;;
    esac
    dir="$WORK/$rel"
    rm -rf "$dir"
    mkdir -p "$(dirname "$dir")"
    cp -r "$src" "$dir"
    rm -rf "$dir/build"
    log="$WORK/$cname.log"

    if [ -f "$src/expect_error.txt" ]; then
        if ( cd "$dir" && "$SINTER" build -j 2 ) >"$log" 2>&1; then
            note "FAIL" "$cname: 应当被拒绝却构建成功"
            fail=$((fail + 1))
            return
        fi
        bad=""
        while IFS= read -r needle; do
            [ -n "$needle" ] || continue
            grep -qF -- "$needle" "$log" || bad="$bad [$needle]"
        done < "$src/expect_error.txt"
        if [ -z "$bad" ]; then
            note "PASS" "$cname (反向：诊断命中)"
            pass=$((pass + 1))
        else
            note "FAIL" "$cname: 诊断缺少 $bad"
            fail=$((fail + 1))
        fi
        return
    fi

    if ! ( cd "$dir" && "$SINTER" build --run -j 2 ) >"$log" 2>&1; then
        note "FAIL" "$cname: 构建或运行失败（$log）"
        fail=$((fail + 1))
        return
    fi
    if ! pedbad=$(check_pedantic "$dir"); then
        note "FAIL" "$cname: 生成的 C 未通过 -std=c17 -pedantic-errors$pedbad"
        fail=$((fail + 1))
        return
    fi
    got="$dir/got.txt"
    tr -d '\000' < "$log" | sed -n '/^----  *[^ ]*  *----$/,$p' | sed '1d' > "$got"
    if ! diff -u "$src/expect.txt" "$got" > "$dir/diff.txt" 2>&1; then
        note "FAIL" "$cname: 输出与 expect.txt 不一致（$dir/diff.txt）"
        fail=$((fail + 1))
        return
    fi
    if [ -f "$src/assert_gen.txt" ]; then
        if ! genbad=$(check_gen "$dir" "$src/assert_gen.txt"); then
            note "FAIL" "$cname: 生成 C 断言未命中$genbad"
            fail=$((fail + 1))
            return
        fi
    fi
    note "PASS" "$cname"
    pass=$((pass + 1))
}

for name in hello collections broken; do
    check_one "$name" "$ROOT/examples/$name"
done

for d in "$ROOT"/tests/cases/*; do
    [ -d "$d" ] || continue
    check_one "$(basename "$d")" "$d"
done

# ---- self-compiling seed ------------------------------------------------
if [ -z "${SINTER_SKIP_SEED:-}" ] && [ -d "$ROOT/src/seed" ] && [ -f "$ROOT/tests/check_seed.sh" ]; then
    if sh "$ROOT/tests/check_seed.sh" "$SINTER" "$SINTERC" > "$WORK/seed.log" 2>&1; then
        note PASS "sinterc seed (self-compiling fixpoint)"
        pass=$((pass + 1))
    else
        note FAIL "sinterc seed（$WORK/seed.log）"
        tail -20 "$WORK/seed.log"
        fail=$((fail + 1))
    fi
elif [ -f "$ROOT/tests/check_seed.sh" ]; then
    note "SKIP" "sinterc seed (缺少 src/seed 目录)"
fi

# ---- self-host bootstrap -------------------------------------------------
if [ -z "${SINTER_SKIP_SELFHOST:-}" ] && [ -d "$ROOT/selfhost" ] && [ -f "$ROOT/tests/check_selfhost.sh" ]; then
    if sh "$ROOT/tests/check_selfhost.sh" "$SINTER" "$SINTERC" > "$WORK/selfhost.log" 2>&1; then
        note PASS "selfhost (frontend + C backend + LSP)"
        pass=$((pass + 1))
    else
        note FAIL "selfhost（$WORK/selfhost.log）"
        tail -20 "$WORK/selfhost.log"
        fail=$((fail + 1))
    fi
elif [ -f "$ROOT/tests/check_selfhost.sh" ]; then
    note "SKIP" "selfhost (缺少 selfhost 目录)"
fi

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]