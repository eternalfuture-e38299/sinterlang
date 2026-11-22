// sinter-lsp: main.cpp —— command line entry
//
// usage:
//   sinter-lsp [--stdio] [--backend /tmp/sinterd.sock] [--verbose] [--self-test]
//
// The only transport implemented today is stdio; `--stdio` is accepted so that
// editor configs copied from other language servers keep working.
#include "server.hpp"

#include "json.hpp"

#include <sinter/version.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
    void print_usage(const char *argv0) {
        std::printf(
            "sinter-lsp %s - Sinter language service (LSP over stdio)\n"
            "\n"
            "usage: %s [options]\n"
            "  --stdio              use stdio (default, and currently the only transport)\n"
            "  --backend PATH       Unix socket of sinterd (used for `embed C` blocks)\n"
            "  --verbose, -v        dump protocol traffic to stderr\n"
            "  --self-test          run the built-in self-test and exit\n"
            "  --version, -V        print the version and exit\n"
            "  --help, -h           show this help\n"
            "\n"
            "Supported capabilities: diagnostics (publishDiagnostics), completion,\n"
            "definition, hover, document symbols.\n",
            SINTER_VERSION_STRING, argv0);
    }
} // namespace

///   self_test.cpp
int run_self_test();

int main(int argc, char **argv) {
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (a == "--self-test") return run_self_test();
        if (a == "--version" || a == "-V") {
            std::printf("sinter-lsp %s\n", SINTER_VERSION_STRING);
            return 0;
        }
        if (a == "--stdio") continue;
        if (a == "--verbose" || a == "-v") {
            verbose = true;
            continue;
        }
        if (a == "--backend") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "sinter-lsp: --backend requires a socket path\n");
                return 2;
            }
            sinterlsp::backend().socket_path = argv[++i];
            continue;
        }
        std::fprintf(stderr, "sinter-lsp: unknown argument '%s' (see --help)\n", a.c_str());
        return 2;
    }
    (void) verbose;

    sinterlsp::Server server;
    return server.run();
}