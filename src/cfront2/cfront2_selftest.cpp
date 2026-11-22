// cfront2 self-test：不依赖外部头文件，覆盖 C99 声明/声明符/语句/表达式。
#include "cfront2/c2_lexer.hpp"
#include "cfront2/c2_parser.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int failures = 0;

static std::string read_file(const char *path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void check(const char *what, bool ok, const std::string &detail = {}) {
    if (!ok) {
        ++failures;
        std::printf("  [FAIL] %s\n", what);
        if (!detail.empty()) std::printf("         %s\n", detail.c_str());
    } else {
        std::printf("  [ OK ] %s\n", what);
    }
}

int main(int argc, char **argv) {
    using namespace sinter::cfront2;
    if (argc > 1) {
        // 手工校验模式：cfront2_selftest file.c [file2.c ...]
        int bad = 0;
        for (int i = 1; i < argc; ++i) {
            Unit u = parse(read_file(argv[i]));
            for (auto &d: u.diagnostics) {
                std::printf("%s:%d:%d: error: %s\n", argv[i], d.line, d.col, d.message.c_str());
                ++bad;
            }
            std::printf("%s: %zu functions, %zu records, %zu typedefs, %zu includes\n", argv[i],
                        u.functions.size(), u.records.size(), u.typedefs.size(), u.includes.size());
        }
        return bad == 0 ? 0 : 1;
    }
    const std::string src =
        "#include <stdint.h>\n"
        "typedef unsigned long size2;\n"
        "struct Point { int x; int y; };\n"
        "enum Color { RED, GREEN = 2, BLUE };\n"
        "static int add(int a, int b) { return a + b; }\n"
        "const char *name(void) { return \"sinter\"; }\n"
        "int (*fnptr)(int) = 0;\n"
        "int arr[3] = {1, 2, 3};\n"
        "void demo(void) {\n"
        "  for (int i = 0; i < 3; ++i) { if (i) continue; }\n"
        "  while (0) { break; }\n"
        "  do { } while (0);\n"
        "  switch (1) { case 1: break; default: break; }\n"
        "  int x = sizeof(struct Point);\n"
        "  int y = (int)(x ? 1 : 2);\n"
        "  (void)y;\n"
        "}\n";
    Unit u = parse(src);
    check("no diagnostics", u.diagnostics.empty(),
          u.diagnostics.empty() ? "" : u.diagnostics.front().message);
    check("include recorded", !u.includes.empty() && u.includes.front() == "stdint.h");
    check("struct Point", !u.records.empty() &&
                              u.records.front().name == "Point" &&
                              u.records.front().fields.size() == 2);
    check("enum variants", !u.records.empty() &&
                               u.records.back().variants.size() == 3);
    check("typedef", !u.typedefs.empty() && u.typedefs.front().name == "size2");
    bool add = false, name = false, demo = false;
    for (auto &f: u.functions) {
        if (f.name == "add") add = true;
        if (f.name == "name") name = true;
        if (f.name == "demo") demo = true;
    }
    check("function add", add);
    check("function name", name);
    check("function demo", demo);
    if (failures) {
        std::printf("cfront2 self-test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("cfront2 self-test: all passed\n");
    return 0;
}
