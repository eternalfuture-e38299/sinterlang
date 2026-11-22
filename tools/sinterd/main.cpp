// sinterd: main.cpp —— commandline
//
// usage：
//   sinterd [--socket PATH] [--verbose] [--foreground] [--idle-timeout SEC]
//           [--self-test]
//
// ：default** **line。process（fork/setsid）will process erroroutput，
//  （、）requiresto  bind failedclass 。
//  --foreground is declaration，defaultlineis   ； call
// （`sinterd &`  or   spawn）。
#include "server.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace {
    void print_usage(const char *argv0) {
        std::printf(
            "sinterd %s - Sinter language backend daemon (formatting + LSP support)\n"
            "\n"
            "usage: %s [options]\n"
            "  --socket PATH        Unix socket path (default %s)\n"
            "  --verbose, -v        log requests to stderr\n"
            "  --foreground         run in foreground (the daemon never forks by itself;\n"
            "                       this flag exists so callers can be explicit about it)\n"
            "  --idle-timeout SEC   exit after SEC idle seconds (0 = stay resident, default 0)\n"
            "  --self-test          run the built-in self-test and exit (no socket needed)\n"
            "  --version, -V        print the version and exit\n"
            "  --help, -h           show this help\n"
            "\n"
            "protocol: one JSON request per line, one JSON response per line.\n"
            "  request {\"id\":\"1\",\"op\":\"format\",\"text\":\"fun main(): void {}\"}\n"
            "  ops: ping / format / reindent / cparse / cparse2 / symbols / symbols2 / diagnose / tokens / shutdown\n",
            sinterd::kVersion, argv0, sinterd::kDefaultSocket);
    }

    /// self-test：not  socket， handle_request_line，
    ///  no  socket file   protocol and op。
    int run_self_test() {
        int failed = 0;
        int total = 0;
        auto expect = [&failed, &total](const char *what, const std::string &got,
                                        const std::string &want) {
            ++total;
            if (got.find(want) == std::string::npos) {
                std::printf("  [FAIL] %s\n         expectedpackage: %s\n         actual: %s\n",
                            what, want.c_str(), got.c_str());
                ++failed;
            } else {
                std::printf("  [ OK ] %s\n", what);
            }
        };
        auto expect_not = [&failed, &total](const char *what, const std::string &got,
                                            const std::string &bad) {
            ++total;
            if (got.find(bad) != std::string::npos) {
                std::printf("  [FAIL] %s\n         not package: %s\n         actual: %s\n",
                            what, bad.c_str(), got.c_str());
                ++failed;
            } else {
                std::printf("  [ OK ] %s\n", what);
            }
        };

        std::printf("sinterd self-test\n");

        // 1. ping
        expect("ping return  pong",
               sinterd::handle_request_line("{\"id\":\"1\",\"op\":\"ping\"}"),
               "\"pong\":true");

        // 2. unknown op
        expect("unknown op",
               sinterd::handle_request_line("{\"id\":\"2\",\"op\":\"nope\"}"),
               "\"ok\":false");

        // 3. format：Sinter 源码。C 的 format_c 只用于 embed C 的 reindent。
        const std::string fmt = sinterd::handle_request_line(
            "{\"id\":\"3\",\"op\":\"format\",\"text\":\"fun main(): void{\\nret 0\\n}\"}");
        expect("format 签名", fmt, "fun main(): void {");
        expect("format 缩进", fmt, "    ret 0");

        // 4. reindent
        expect("reindent ",
               sinterd::handle_request_line(
                   "{\"id\":\"4\",\"op\":\"reindent\",\"text\":\"if(x){\\ny();\\n}\"}"),
               "    y();");

        // 4b. `if (` 留空格，函数调用 `y()` 紧贴
        const std::string fmt2 = sinterd::handle_request_line(
            "{\"id\":\"4b\",\"op\":\"format\",\"text\":\"fun main(): void{\\nif(x){y()}\\n}\"}");
        expect("format `if (` ", fmt2, "if (x)");
        expect_not("format  y ()", fmt2, "y ()");

        // 4c. Sinter 专有符号：`=>` 不能被拆成 `= >`，默认构造保持原样
        const std::string fmt3 = sinterd::handle_request_line(
            "{\"id\":\"4c\",\"op\":\"format\",\"text\":\"pub class Any {\\nAny() = default\\n"
            "fun f(): i64 => 1\\n}\"}");
        expect("format = default", fmt3, "Any() = default");
        expect("format =>", fmt3, "fun f(): i64 => 1");
        expect_not("format => 被拆开", fmt3, "= >");

        // 4d. 一元负号不能变成二元表达式
        const std::string fmt4 = sinterd::handle_request_line(
            "{\"id\":\"4d\",\"op\":\"format\",\"text\":\"fun f(): i64 {\\nret -1\\n}\\n"
            "fun g(): i64 => Int(-1)\"}");
        expect("format ret -1", fmt4, "ret -1");
        expect("format Int(-1)", fmt4, "Int(-1)");

        // 4e. 文档注释原样，连续空行压成一行
        const std::string fmt5 = sinterd::handle_request_line(
            "{\"id\":\"4e\",\"op\":\"format\",\"text\":\"/// doc\\n/** block doc */\\n"
            "fun f(): void {\\n}\\n\\n\\nfun g(): void {\\n}\"}");
        expect("format /// 文档注释", fmt5, "/// doc");
        expect("format /** */ 文档注释", fmt5, "/** block doc */");
        expect("format 空行压缩", fmt5, "}\\n\\nfun g(): void");

        // 5. cparse to function
        expect("cparse to function",
               sinterd::handle_request_line(
                   "{\"id\":\"5\",\"op\":\"cparse\",\"text\":\"int add(int a,int b){return a+b;}\"}"),
               "\"functions\":[\"add\"]");
        // 5b. 第二个 C 前端（独立 cfront2）
        const std::string p2 = sinterd::handle_request_line(
            "{\"id\":\"5b\",\"op\":\"cparse2\",\"text\":\"typedef int T; struct P{int x;};"
            "int add(int a,int b){return a+b;}\"}");
        expect("cparse2 frontend", p2, "\"frontend\":\"cfront2\"");
        expect("cparse2 function", p2, "\"name\":\"add\"");
        expect("cparse2 struct", p2, "\"name\":\"P\"");
        expect("cparse2 no diagnostics", p2, "\"diagnostics\":[]");

        // 6. cparse to  include
        expect("cparse to  include",
               sinterd::handle_request_line(
                   "{\"id\":\"6\",\"op\":\"cparse\",\"text\":\"#include <stdio.h>\\nint main(void){return 0;}\"}"),
               "stdio.h");

        // 7. diagnose：codeno diagnostic
        expect("diagnose codeis ",
               sinterd::handle_request_line(
                   "{\"id\":\"7\",\"op\":\"diagnose\",\"text\":\"int main(void){return 0;}\"}"),
               "\"diagnostics\":[]");

        // 8. diagnose：syntaxerrormust
        const std::string dg = sinterd::handle_request_line(
            "{\"id\":\"8\",\"op\":\"diagnose\",\"text\":\"int main(void){ int x = ; }\"}");
        expect_not("diagnose errorcode", dg, "\"diagnostics\":[]");

        // 9. tokens：mustto offset and linecolumn
        const std::string tk = sinterd::handle_request_line(
            "{\"id\":\"9\",\"op\":\"tokens\",\"text\":\"int x;\"}");
        expect("tokens  keyword", tk, "\"kind\":\"keyword\"");
        expect("tokens  byte offset", tk, "\"offset\":0");

        // 10. invalid JSON
        expect("invalid JSON ",
               sinterd::handle_request_line("{not json}"),
               "\"ok\":false");

        // 11. id must
        expect("id ",
               sinterd::handle_request_line("{\"id\":\"abc\",\"op\":\"ping\"}"),
               "\"id\":\"abc\"");

        // 11b. number id  ，and must number（not  ）。
        //        str_value，number id will  ，client  not response。
        expect("number id ",
               sinterd::handle_request_line("{\"id\":7,\"op\":\"ping\"}"),
               "\"id\":7,");
        expect_not("number id not   ",
                   sinterd::handle_request_line("{\"id\":7,\"op\":\"ping\"}"),
                   "\"id\":\"7\"");
        expect("string id  ",
               sinterd::handle_request_line("{\"id\":\"x7\",\"op\":\"ping\"}"),
               "\"id\":\"x7\",");

        // 11c. sourcefield  text  and  source（ ）
        expect("text field ",
               sinterd::handle_request_line(
                   "{\"id\":\"t1\",\"op\":\"format\",\"text\":\"int f(){return 0;}\"}"),
               "int f()");
        expect("source field ",
               sinterd::handle_request_line(
                   "{\"id\":\"t2\",\"op\":\"format\",\"source\":\"int f(){return 0;}\"}"),
               "int f()");
        expect("source field  tokens ",
               sinterd::handle_request_line(
                   "{\"id\":\"t3\",\"op\":\"tokens\",\"source\":\"int f(){} \"}"),
               "\"tokens\":[{");

        // 12. empty request / invalid JSON errornot  ，and  id is  null
        expect("empty request", sinterd::handle_request_line(""), "\"ok\":false");
        expect("invalid JSON ", sinterd::handle_request_line("not json"), "\"ok\":false");
        expect(" op ", sinterd::handle_request_line("{\"id\":1}"), "\"ok\":false");

        if (failed == 0) std::printf("all passed（%d ）\n", total);
        else std::printf("failed %d / %d \n", failed, total);
        return failed == 0 ? 0 : 1;
    }
} // namespace

int main(int argc, char **argv) {
    sinterd::ServerConfig cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (a == "--self-test") {
            return run_self_test();
        }
        if (a == "--version" || a == "-V") {
            std::printf("sinterd %s\n", sinterd::kVersion);
            return 0;
        }
        if (a == "--verbose" || a == "-v") {
            cfg.verbose = true;
            continue;
        }
        if (a == "--foreground") {
            // defaulti.e.  ；
            continue;
        }
        if (a == "--socket") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "sinterd: --socket requires a path argument\n");
                return 2;
            }
            cfg.socket_path = argv[++i];
            continue;
        }
        if (a == "--idle-timeout") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "sinterd: --idle-timeout expects a number of seconds\n");
                return 2;
            }
            cfg.idle_timeout_sec = std::atoi(argv[++i]);
            continue;
        }
        std::fprintf(stderr, "sinterd: unknown argument '%s' (see --help)\n", a.c_str());
        return 2;
    }

    return sinterd::run_server(cfg);
}