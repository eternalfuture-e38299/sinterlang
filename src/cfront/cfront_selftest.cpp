// cfront_selftest —— C 前端自测（不依赖外部测试框架）
//
// 目的：验证 cfront 能真正解析 embed C 块里的那种 C 代码，
// 并把函数、struct、typedef、include 正确收集出来。
// 用法：cfront_selftest [<file.c>]，不给文件时跑内置用例。
#include "cfront/c_parser.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace sinter::cfront;

namespace {
    int g_pass = 0;
    int g_fail = 0;

    void expect(const bool cond, const std::string &what) {
        if (cond) {
            ++g_pass;
        } else {
            ++g_fail;
            std::cout << "[FAIL] " << what << "\n";
        }
    }

    bool has(const std::vector<std::string> &v, const std::string &s) {
        for (const auto &x: v)
            if (x == s) return true;
        return false;
    }

    std::string join(const std::vector<std::string> &v) {
        std::string r;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) r += ' ';
            r += v[i];
        }
        return r;
    }

    // ---- 用例 1：词法 ----
    void test_lexer() {
        CLexer lx("int x = 0x1Fu + 3.5e2f; const char* s = \"a\\\"b\";");
        auto r = lx.tokenize();
        expect(r.errors.empty(), "词法：无错误");
        // int x = 0x1Fu + 3.5e2f ; const char * s = "a\"b" ; EOF
        expect(r.tokens.size() == 15, "词法：token 数量 15，实际 " +
                                            std::to_string(r.tokens.size()));
        expect(r.tokens[0].kind == CTokKind::KwInt, "词法：int 识别为关键字");
        expect(r.tokens[3].kind == CTokKind::IntLiteral, "词法：0x1Fu 识别为整数");
        expect(r.tokens[3].text == "0x1Fu", "词法：0x1Fu 原文保留");
        expect(r.tokens[5].kind == CTokKind::FloatLiteral, "词法：3.5e2f 识别为浮点");
        expect(r.tokens[5].text == "3.5e2f", "词法：3.5e2f 原文保留");
    }

    void test_lexer_preproc() {
        CLexer lx("#include <stdio.h>\n#define MAX 10\nint x;\n");
        auto r = lx.tokenize();
        expect(r.errors.empty(), "预处理：无错误");
        expect(r.tokens[0].kind == CTokKind::PreprocLine, "预处理：#include 合并为一行");
        expect(r.tokens[0].directive == "include", "预处理：指令名 include");
        expect(r.tokens[1].kind == CTokKind::PreprocLine, "预处理：#define 合并为一行");
        expect(r.tokens[1].directive == "define", "预处理：指令名 define");
        expect(r.tokens[2].kind == CTokKind::KwInt, "预处理：后续代码正常词法化");
    }

    void test_lexer_continuation() {
        CLexer lx("#define BIG(x) \\\n    ((x) * 2)\nint y;\n");
        auto r = lx.tokenize();
        expect(r.tokens[0].kind == CTokKind::PreprocLine, "续行：合并为一条预处理");
        expect(r.tokens[0].text.find("(x) * 2") != std::string::npos, "续行：内容包含第二行");
        expect(r.tokens[1].kind == CTokKind::KwInt, "续行:之后代码正常");
    }

    // ---- 用例 2：函数定义 ----
    void test_function() {
        const std::string src = R"(
static inline int add(int a, int b) {
    return a + b;
}
int main(void) {
    return add(1, 2);
}
)";
        auto r = CParser::parse(src, "t.c");
        expect(r.errors.empty(), "函数：解析无错误，" + std::to_string(r.errors.size()) + " 个");
        const auto fns = r.unit.function_names();
        expect(has(fns, "add"), "函数：收集到 add");
        expect(has(fns, "main"), "函数：收集到 main");
        expect(r.unit.items.size() == 2, "函数：2 个顶层条目，实际 " +
                                             std::to_string(r.unit.items.size()));
        // static inline 标志
        expect(r.unit.items[0].func->decl.is_static, "函数：static 标志");
        expect(r.unit.items[0].func->decl.is_inline, "函数：inline 标志");
        // 参数
        const auto &d = r.unit.items[0].func->decl.decl;
        expect(d && d->kind == CDeclarator::Kind::Function, "函数：声明符是 Function");
        expect(d && d->params.size() == 2, "函数：2 个形参");
        expect(d && d->params[0].name() == "a", "函数：形参名 a");
        expect(d && d->params[1].name() == "b", "函数：形参名 b");
        expect(r.unit.items[0].func->decl.spec.canonical() == "int", "函数：返回类型 int");
    }

    // ---- 用例 3：函数指针（C 声明符优先级的试金石）----
    void test_func_ptr() {
        const std::string src = "int (*handler)(int);\n";
        auto r = CParser::parse(src, "t.c");
        expect(r.errors.empty(), "函数指针：解析无错误");
        expect(r.unit.items.size() == 1, "函数指针：1 个条目");
        const auto &d = r.unit.items[0].decl.decl;
        expect(d != nullptr, "函数指针：有声明符");
        expect(d && d->kind == CDeclarator::Kind::Function, "函数指针：外层是 Function");
        expect(d && d->inner && d->inner->kind == CDeclarator::Kind::Paren,
               "函数指针：内层是 Paren");
        expect(d && d->inner && d->inner->inner &&
                       d->inner->inner->kind == CDeclarator::Kind::Pointer,
               "函数指针：Paren 内是 Pointer");
        expect(d && d->leaf_name() == "handler", "函数指针：名字 handler");
    }

    void test_array_of_func_ptr() {
        const std::string src = "void (*table[4])(void);\n";
        auto r = CParser::parse(src, "t.c");
        expect(r.errors.empty(), "函数指针数组：解析无错误");
        expect(r.unit.items.size() == 1, "函数指针数组：1 个条目");
        expect(r.unit.items[0].decl.name() == "table", "函数指针数组：名字 table");
    }

    // ---- 用例 4：struct / enum / typedef ----
    void test_struct() {
        const std::string src = R"(
typedef struct Point {
    int x;
    int y;
} Point;

enum Color { RED, GREEN = 5, BLUE };

typedef unsigned long size_type;
)";
        auto r = CParser::parse(src, "t.c");
        expect(r.errors.empty(), "结构体：解析无错误");
        const auto tags = r.unit.tag_names();
        expect(has(tags, "Point"), "结构体：收集到标签 Point");
        expect(has(tags, "Color"), "结构体：收集到标签 Color");
        const auto tds = r.unit.typedef_names();
        expect(has(tds, "Point"), "结构体：typedef Point");
        expect(has(tds, "size_type"), "结构体：typedef size_type");
    }

    // ---- 用例 5：语句覆盖 ----
    void test_statements() {
        const std::string src = R"(
int f(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        if (i % 2 == 0) {
            continue;
        } else {
            sum += i;
        }
    }
    switch (n) {
        case 1: sum = 10; break;
        default: sum = 20;
    }
    do { sum--; } while (sum > 0);
    while (n > 0) { n--; if (n == 3) break; }
    return sum;
}
)";
        auto r = CParser::parse(src, "t.c");
        expect(r.errors.empty(), "语句：解析无错误，错误数 " +
                                     std::to_string(r.errors.size()));
        expect(has(r.unit.function_names(), "f"), "语句：收集到 f");
    }

    // ---- 用例 6：表达式覆盖 ----
    void test_expressions() {
        const std::string src = R"(
int g(int a, int b) {
    int c = a > b ? a : b;
    int d = (int)(a * 1.5) + sizeof(long);
    int e = a << 2 | b & 0xFF;
    struct P { int x; } p;
    p.x = 1;
    int arr[3];
    arr[0] = c + d + e;
    return arr[0];
}
)";
        auto r = CParser::parse(src, "t.c");
        expect(r.errors.empty(), "表达式：解析无错误，错误数 " +
                                     std::to_string(r.errors.size()));
        expect(has(r.unit.function_names(), "g"), "表达式：收集到 g");
    }

    // ---- 用例 7：include 收集 ----
    void test_includes() {
        const std::string src = "#include <stdio.h>\n#include \"local.h\"\nint x;\n";
        auto r = CParser::parse(src, "t.c");
        const auto incs = r.unit.includes();
        expect(has(incs, "stdio.h"), "include：收集到 stdio.h");
        expect(has(incs, "local.h"), "include：收集到 local.h");
    }

    // ---- 用例 8：容错（LSP 编辑中代码不完整）----
    void test_recovery() {
        const std::string src = R"(
int good(void) { return 1; }
int broken(void) {
    int x = ;
    return x
}
int after(void) { return 3; }
)";
        auto r = CParser::parse(src, "t.c");
        expect(!r.errors.empty(), "容错：不完整代码应报错");
        // 关键：坏函数之后的函数仍应被收集
        expect(has(r.unit.function_names(), "after"), "容错：错误后仍能收集 after");
        expect(has(r.unit.function_names(), "good"), "容错：错误前能收集 good");
    }


    // ---- 用例：错误归属到 #include 进来的文件 ----
    // 真实头文件几乎都是「外层头 include 一堆内层头」，如果错误全算到外层头头上，
    // 行列对得上、文件名是错的，反而比不报更难查。
    void test_file_attribution() {
        const std::string hdr = "/tmp/cfront_attr_probe.h";
        {
            std::ofstream out(hdr);
            out << "struct bad { int a b; };\n";
        }
        CParseOptions opt;
        opt.include_dirs.push_back("/tmp");
        const std::string main_path = "/tmp/cfront_attr_main.c";
        // 用定义（而不是声明）作后续内容：function_names() 只收 FunctionDef，
        // 声明走 CUnitItem::Kind::Declaration，断言会误判。
        auto r = CParser::parse("#include <cfront_attr_probe.h>\nint ok_after(void) { return 7; }\n",
                                main_path, opt);
        expect(!r.errors.empty(), "归属：被包含文件的语法错误被报出");
        expect(!r.files.empty() && r.files[0] == main_path, "归属：files[0] 是主文件");
        bool tagged = false;
        for (const auto &e: r.errors)
            if (e.file == hdr) tagged = true;
        expect(tagged, "归属：错误带上了被包含文件的路径");
        expect(has(r.unit.function_names(), "ok_after"), "归属：子头出错后主文件仍能继续解析");
    }

    // ---- 用例：预处理（条件编译 / 宏 / 头文件保护） ----
    void test_preproc() {
        const std::string src = R"(
#define ENABLED 1
#define SQR(x) ((x) * (x))
#if ENABLED && !defined(DISABLED)
int on_enabled(void) { return SQR(3); }
#else
int on_disabled(void) { return 0; }
#endif
#ifdef DISABLED
int never(void) { return 1; }
#endif
)";
        auto r = CParser::parse(src, "preproc.c");
        expect(r.errors.empty() && r.preproc_errors.empty(), "预处理：条件编译无错误");
        const auto fns = r.unit.function_names();
        expect(has(fns, "on_enabled"), "预处理：#if 命中分支被收集");
        expect(!has(fns, "on_disabled"), "预处理：#else 分支被剔除");
        expect(!has(fns, "never"), "预处理：#ifdef 未定义时整段剔除");
    }

    // ---- 用例：extern "C" 链接块 ----
    void test_extern_c_block() {
        const std::string src = R"(
#ifdef __cplusplus
extern "C" {
#endif
int c_api(int x);
#ifdef __cplusplus
}
#endif
)";
        CParseOptions opt;
        opt.predefined.push_back({"__cplusplus", "1"});
        auto r = CParser::parse(src, "extern_c.h", opt);
        expect(r.errors.empty(), "extern C：块内声明无解析错误");
        bool found = false;
        for (auto &item: r.unit.items)
            if (item.kind == CUnitItem::Kind::Declaration && item.decl.name() == "c_api") found = true;
        expect(found, "extern C：块内声明被收集");
    }

    // ---- 用例：扩展说明符 / 静态断言 ----
    void test_attributes() {
        const std::string src = R"(
__attribute__((visibility("default"))) int g(void);
__declspec(dllexport) int h(void);
int k(void) __attribute__((noreturn));
_Static_assert(sizeof(int) == 4, "int must be 4 bytes");
)";
        auto r = CParser::parse(src, "attrs.h");
        expect(r.errors.empty(), "属性：__attribute__ / __declspec / _Static_assert 无解析错误");
        bool sa = false;
        for (auto &it: r.unit.items)
            if (it.kind == CUnitItem::Kind::StaticAssert) sa = true;
        expect(sa, "属性：_Static_assert 被收集");
    }

    // ---- 用例：初始化列表 + 指定初始化 ----
    void test_initializer_list() {
        const std::string src = R"(
struct P { int x; int y; };
static const struct P origin = { .x = 0, .y = 0 };
static const int table[] = { [1] = 10, [3] = 30 };
)";
        auto r = CParser::parse(src, "init.c");
        expect(r.errors.empty(), "初始化列表：位置 / 指定初始化无错误");
        expect(r.unit.items.size() >= 3, "初始化列表：条目被收集");
        bool has_designator = false;
        for (auto &item: r.unit.items) {
            if (!item.decl.init) continue;
            if (item.decl.init->kind != CExpr::Kind::InitList) continue;
            for (auto &el: item.decl.init->init_elems)
                if (!el.designator.empty()) has_designator = true;
        }
        expect(has_designator, "初始化列表：指示符被保留");
    }

    // ---- 用例：位域 ----
    void test_bitfield() {
        const std::string src = R"(
struct Flags { unsigned ready : 1; unsigned mode : 3; unsigned : 4; };
)";
        auto r = CParser::parse(src, "bits.c");
        expect(r.errors.empty(), "位域：解析无错误");
        bool width_ok = false;
        for (auto &item: r.unit.items) {
            if (!item.decl.spec.struct_def) continue;
            for (auto &fd: item.decl.spec.struct_def->fields)
                if (fd.name() == "mode" && fd.bit_width == "3") width_ok = true;
        }
        expect(width_ok, "位域：mode 的宽度被记为 3");
    }

    void test_empty() {
        auto r = CParser::parse("", "empty.c");
        expect(r.errors.empty(), "空文件：无错误");
        expect(r.unit.items.empty(), "空文件：无条目");
    }
    /// `--parse <file>... [-I<dir>]`：把真实头文件扫一遍，报告条目数与错误。
    /// 不进 ctest —— 它依赖宿主 `/usr/include`；但它是「解析器对真实世界的覆盖」
    /// 这条结论的唯一来源，所以做成可复现的命令而非常驻测试。
    int parse_files(int argc, char **argv) {
        CParseOptions opt;
        std::vector<std::string> files;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a.rfind("-I", 0) == 0) opt.include_dirs.push_back(a.substr(2));
            else files.push_back(a);
        }
        int bad = 0;
        for (const auto &path: files) {
            std::ifstream f(path);
            if (!f) {
                std::cout << "[FAIL] cannot open " << path << "\n";
                ++bad;
                continue;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            const auto r = CParser::parse(ss.str(), path, opt);
            std::cout << path << "  items=" << r.unit.items.size()
                      << " errors=" << r.errors.size()
                      << " preproc_errors=" << r.preproc_errors.size() << "\n";
            for (size_t k = 0; k < r.errors.size() && k < 5; ++k)
                std::cout << "   ! " << (r.errors[k].file.empty() ? path : r.errors[k].file)
                          << ":" << r.errors[k].line << ":" << r.errors[k].column << " "
                          << r.errors[k].message << "\n";
            if (!r.errors.empty()) ++bad;
        }
        std::cout << (bad == 0 ? "all headers parsed clean\n" : "some headers failed\n");
        return bad == 0 ? 0 : 1;
    }
} // namespace

int main(int argc, char **argv) {
    if (argc > 2 && std::string(argv[1]) == "--parse") return parse_files(argc, argv);
    // 自测输出不缓冲：这样在「某个用例把进程挂住」时还能看到它跑到哪一步。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout << "[cfront] selftest start\n";
    test_lexer();
    std::cout << "[cfront] lexer done\n";
    test_lexer_preproc();
    test_lexer_continuation();
    std::cout << "[cfront] preproc done\n";
    test_function();
    test_func_ptr();
    test_array_of_func_ptr();
    test_struct();
    test_statements();
    test_expressions();
    test_includes();
    std::cout << "[cfront] core done\n";
    test_file_attribution();
    std::cout << "[cfront] include attribution done\n";
    test_preproc();
    test_extern_c_block();
    test_attributes();
    test_initializer_list();
    test_bitfield();
    std::cout << "[cfront] new features done\n";
    test_recovery();
    test_empty();

    // 额外：若给了文件，就真实解析它（用于验证 std.sinter 里的 embed C 正文）
    if (argc > 1) {
        std::ifstream f(argv[1]);
        if (!f) {
            std::cout << "[FAIL] 无法打开 " << argv[1] << "\n";
            ++g_fail;
        } else {
            std::ostringstream ss;
            ss << f.rdbuf();
            const std::string src = ss.str();
            // Sinter 自身的类型（typedef struct sinter_string）不在 embed C 块内，
            // 由调用方作为已知 typedef 传入——这正是 parse_with_typedefs 的用途。
            const std::set<std::string> extra = {"sinter_string"};
            auto r = CParser::parse_with_typedefs(src, extra, argv[1]);
            std::cout << "\n=== 解析 " << argv[1] << " ===\n"
                      << "  顶层条目: " << r.unit.items.size() << "\n"
                      << "  函数: " << r.unit.function_names().size() << "\n"
                      << "  struct/enum: " << r.unit.tag_names().size() << "\n"
                      << "  typedef: " << r.unit.typedef_names().size() << "\n"
                      << "  include: " << join(r.unit.includes()) << "\n"
                      << "  错误数: " << r.errors.size() << "\n";
            for (size_t i = 0; i < r.errors.size() && i < 10; ++i)
                std::cout << "    " << r.errors[i].line << ":" << r.errors[i].column << " "
                          << r.errors[i].message << "\n";
            expect(r.errors.empty(), "真实文件：解析无错误");
        }
    }

    std::cout << "\n通过 " << g_pass << "，失败 " << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}