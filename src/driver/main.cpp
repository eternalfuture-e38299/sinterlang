// sinter driver
#include "front/lexer.hpp"
#include <sinter/version.hpp>
#include <functional>
#include "front/parser.hpp"
#include "front/generic_mono.hpp"
#include "front/comptime.hpp"
#include "c_bridge.hpp"
#include "sem/semantic.hpp"
#include "sem/typeck.hpp"
#include "sem/ownership.hpp"
#include "backend/codegen.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

namespace fs = std::filesystem;

namespace {
    struct ModuleUnit {
        std::string module_name;
        fs::path source_path;
        fs::path output_c_path;
        fs::path output_h_path;
        sinter::Program program;
    };

    class ModuleLoader {
    public:
        void add_search_dir(const fs::path &dir) { search_dirs_.push_back(dir); }
        void set_output_dir(const fs::path &dir) { output_dir_ = dir; }

        bool load_entry(const fs::path &entry) {
            std::string ignored;
            std::error_code ec;
            const fs::path abs_entry = fs::absolute(entry, ec);
            entry_dir_ = (ec ? entry : abs_entry).parent_path();
            if (!load_module(entry, ignored)) return false;
            // 隐式模块依赖：`math.square(...)` 但没写 `import math`
            // 迭代到不动点，直到没有新模块需要加载。
            for (int guard = 0; guard < 16; ++guard) {
                std::vector<std::string> needed;
                for (auto &m: ordered_) collect_module_uses(m.program, needed);
                bool added = false;
                for (auto &w: needed) {
                    if (w == "std" || w == "self" || w == "this") continue;
                    fs::path target;
                    for (auto &m: ordered_) {
                        if (m.module_name == w) { target = m.source_path; break; }
                    }
                    if (!target.empty()) continue;   // 已加载
                    target = resolve_import(w, entry.parent_path());
                    if (target.empty()) continue;
                    std::string nm;
                    if (!load_module(target, nm)) return false;
                    // 记下「谁引用了哪个模块」，用于给引用方补 import（供 codegen 出 #include）
                    implicit_imports_.push_back({w, entry});
                    added = true;
                }
                if (!added) break;
            }
            // 给引用方补 import（去重）
            for (auto &[dep_module, from_path]: implicit_imports_) {
                for (auto &m: ordered_) {
                    if (m.source_path != from_path) continue;
                    bool has = false;
                    for (auto &st: m.program.statements)
                        if (auto *imp = dynamic_cast<sinter::ImportStmt *>(st.get());
                            imp && imp->path == dep_module)
                            has = true;
                    if (has) continue;
                    auto *imp = new sinter::ImportStmt();
                    imp->path = dep_module;
                    m.program.statements.insert(m.program.statements.begin(),
                                                sinter::StmtPtr(imp));
                }
            }
            return true;
        }

        std::vector<std::pair<std::string, fs::path>> implicit_imports_;

        /// 把 `std.str` / `std` 这类点分模块路径还原成文本
        static std::string dotted_path(const sinter::Expression *e) {
            if (auto *id = dynamic_cast<const sinter::IdentifierExpr *>(e)) return id->name;
            if (auto *m = dynamic_cast<const sinter::MemberAccessExpr *>(e)) {
                const std::string left = dotted_path(m->object.get());
                if (left.empty()) return "";
                return left + "." + m->member;
            }
            return "";
        }

        /// 收集程序里 `mod.fn(...)` 形式使用到的模块名
        static void collect_module_uses(const sinter::Program &prog, std::vector<std::string> &out) {
            std::function<void(const sinter::Expression *)> scan_expr =
                [&](const sinter::Expression *e) {
                    if (!e) return;
                    if (auto *c = dynamic_cast<const sinter::CallExpr *>(e)) {
                        if (auto *ma = dynamic_cast<const sinter::MemberAccessExpr *>(c->callee.get())) {
                            const std::string path = dotted_path(ma->object.get());
                            if (!path.empty()) out.push_back(path);
                        }
                        scan_expr(c->callee.get());
                        for (auto &a: c->args) scan_expr(a.value.get());
                    } else if (auto *b = dynamic_cast<const sinter::BinaryExpr *>(e)) {
                        scan_expr(b->left.get());
                        scan_expr(b->right.get());
                    } else if (auto *g = dynamic_cast<const sinter::GroupingExpr *>(e)) {
                        scan_expr(g->expr.get());
                    } else if (auto *ma = dynamic_cast<const sinter::MemberAccessExpr *>(e)) {
                        scan_expr(ma->object.get());
                    } else if (auto *u = dynamic_cast<const sinter::UnaryExpr *>(e)) {
                        scan_expr(u->operand.get());
                    } else if (auto *t = dynamic_cast<const sinter::TernaryExpr *>(e)) {
                        scan_expr(t->cond.get());
                        scan_expr(t->then_b.get());
                        scan_expr(t->else_b.get());
                    }
                };
            std::function<void(const sinter::Statement *)> scan_stmt =
                [&](const sinter::Statement *st) {
                    if (!st) return;
                    if (auto *b = dynamic_cast<const sinter::BlockStmt *>(st)) {
                        for (auto &x: b->statements) scan_stmt(x.get());
                    } else if (auto *fd = dynamic_cast<const sinter::FunctionDecl *>(st)) {
                        scan_stmt(fd->body.get());
                    } else if (auto *cd = dynamic_cast<const sinter::ClassDecl *>(st)) {
                        for (auto &m: cd->members) scan_stmt(m.get());
                    } else if (auto *pb = dynamic_cast<const sinter::PubBlockStmt *>(st)) {
                        for (auto &m: pb->statements) scan_stmt(m.get());
                    } else if (auto *ms = dynamic_cast<const sinter::ModuleStmt *>(st)) {
                        for (auto &m: ms->body) scan_stmt(m.get());
                    } else if (auto *ex = dynamic_cast<const sinter::ExprStmt *>(st)) {
                        scan_expr(ex->expr.get());
                    } else if (auto *v = dynamic_cast<const sinter::VarDeclStmt *>(st)) {
                        scan_expr(v->init.get());
                    } else if (auto *r = dynamic_cast<const sinter::ReturnStmt *>(st)) {
                        scan_expr(r->value.get());
                    } else if (auto *i = dynamic_cast<const sinter::IfStmt *>(st)) {
                        scan_expr(i->cond.get());
                        scan_stmt(i->then_b.get());
                        scan_stmt(i->else_b.get());
                    } else if (auto *w = dynamic_cast<const sinter::WhileStmt *>(st)) {
                        scan_expr(w->cond.get());
                        scan_stmt(w->body.get());
                    }
                };
            for (auto &st: prog.statements) scan_stmt(st.get());
        }

        std::vector<ModuleUnit> &modules() { return ordered_; }

    private:
        std::vector<fs::path> search_dirs_;
        fs::path output_dir_ = ".";
        /// 入口文件所在目录。子模块的 `import` 既可能写成相对于自己所在目录，
        /// 也可能写成相对于工程入口（`packages/std/src/std/core.sinter` 里写
        /// `import std.core`，指的是同级的 `src/std/core.sinter`）。只按
        /// 「当前文件目录」找会让子目录里的模块找不到兄弟模块。
        fs::path entry_dir_;
        /// 语法错误统一落到这里：Parser 只负责报告（含文件/行列），
        /// 由 load_module 在 catch 里 flush 一次。
        sinter::Diagnostics parse_diags_;
        std::unordered_map<std::string, size_t> loaded_index_;
        std::unordered_set<std::string> visiting_;
        std::vector<ModuleUnit> ordered_;

        static std::string extract_module_name(const sinter::Program &p) {
            for (auto &s: p.statements)
                if (auto *m = dynamic_cast<sinter::ModuleStmt *>(s.get())) return m->name;
            return "";
        }

        fs::path resolve_import(const std::string &import_path, const fs::path &from_dir) const {
            std::string rel = import_path;
            for (auto &ch: rel) if (ch == '.') ch = '/';

            std::vector<fs::path> candidates;
            auto add = [&](const fs::path &base) {
                candidates.push_back(base / (rel + ".sinter"));
                candidates.push_back(base / rel / "index.sinter");
                candidates.push_back(base / (import_path + ".sinter"));
                fs::path dir = base / rel;
                std::string fname = dir.filename().string();
                if (!fname.empty()) candidates.push_back(dir / (fname + ".sinter"));
            };
            add(from_dir);
            // 入口目录也参与搜索：子目录里的模块写 `import std.core` 时，
            // 目标可能在 `src/std/core.sinter`（相对入口）而不是相对自己。
            if (!entry_dir_.empty()) add(entry_dir_);
            for (auto &dir: search_dirs_) add(dir);

            for (auto &c: candidates)
                if (fs::exists(c)) return c;
            return {};
        }

        /// `bind C header "x.h"`：用 C 前端解析头文件，把函数原型与不透明结构体
        /// 转成 AST 节点插到模块最前面（保证先把 C 声明注册进语义层，
        /// 后面同名 `extern fun` 才能与原型做 ABI 比对）。
        bool apply_bind_headers(sinter::Program &prog, const fs::path &from_dir,
                               const std::string &module_file) {
            std::vector<sinter::StmtPtr> injected;
            for (auto &s: prog.statements) {
                auto *h = dynamic_cast<sinter::CBindHeaderStmt *>(s.get());
                if (!h) continue;
                std::vector<fs::path> cands;
                const fs::path rel(h->header);
                if (rel.is_absolute()) cands.push_back(rel);
                else {
                    cands.push_back(from_dir / rel);
                    for (auto &d: search_dirs_) cands.push_back(d / rel);
                }
                fs::path resolved;
                for (auto &c: cands) {
                    std::error_code ec;
                    if (fs::exists(c, ec)) { resolved = c; break; }
                }
                if (resolved.empty()) {
                    std::cerr << module_file << ":" << h->line << ":" << h->col
                              << ": error[E0801]: cannot find C header '" << h->header << "'"
                              << char(10)
                              << "  = hint: add '-I <dir>' pointing at the header, or write a path "
                                 "relative to this .sinter file" << char(10);
                    return false;
                }
                h->resolved = resolved.string();

                sinter_bridge::Options bo;
                for (auto &d: search_dirs_) bo.include_dirs.push_back(d.string());
                bo.include_dirs.push_back(from_dir.string());   // 头文件同目录的相对 #include
                auto res = sinter_bridge::load_header(resolved.string(), h->header, bo);
                if (!res.errors.empty()) {
                    for (auto &e: res.errors) std::cerr << e << char(10);
                    return false;
                }
                for (auto &n: res.nodes) injected.push_back(std::move(n));
            }
            if (!injected.empty())
                prog.statements.insert(prog.statements.begin(),
                                       std::make_move_iterator(injected.begin()),
                                       std::make_move_iterator(injected.end()));
            return true;
        }

        bool load_module(const fs::path &path, std::string &module_name_out) {
            if (std::getenv("SINTER_TRACE")) std::cerr << "[trace] load " << path.string() << "\n";
            std::error_code ec;
            fs::path abs = fs::absolute(path, ec);
            std::string key = ec ? path.string() : abs.string();

            if (auto it = loaded_index_.find(key); it != loaded_index_.end()) {
                module_name_out = ordered_[it->second].module_name;
                return true;
            }
            if (visiting_.contains(key)) {
                std::cerr << "circular import detected: " << path << "\n";
                return false;
            }
            visiting_.insert(key);

            std::ifstream f(path);
            if (!f) { std::cerr << "cannot open " << path << "\n"; return false; }
            std::stringstream ss;
            ss << f.rdbuf();

            sinter::Lexer lex(ss.str());
            sinter::Tokens tokens;
            try {
                tokens = lex.tokenize();
            } catch (const std::exception &e) {
                std::cerr << "lex failed in " << path << ": " << e.what() << "\n";
                return false;
            }

            if (std::getenv("SINTER_TRACE")) std::cerr << "[trace] tokens=" << tokens.size() << "\n";
            sinter::Parser parser(tokens);
            parser.set_source_path(path.string());
            parser.set_diagnostics(&parse_diags_);
            sinter::Program prog;
            try {
                prog = parser.parse();
            } catch (const std::exception &) {
                // 诊断已经带文件/行列/源码行 + ^^^，这里不再重复打印 what()。
                parse_diags_.flush(std::cerr);
                return false;
            }

            // bind C header：尽早解析，失败就不要继续（否则后面会报一堆「未声明的函数」）
            if (!apply_bind_headers(prog, abs.parent_path(), path.string())) return false;

            std::string mod_name = extract_module_name(prog);
            if (mod_name.empty()) mod_name = path.stem().string();
            module_name_out = mod_name;

            for (auto &s: prog.statements) {
                if (auto *imp = dynamic_cast<sinter::ImportStmt *>(s.get())) {
                    fs::path target = resolve_import(imp->path, abs.parent_path());
                    if (target.empty()) {
                        std::cerr << "import not found: " << imp->path
                                  << " (from " << path << ")\n";
                        return false;
                    }
                    std::string ignored;
                    if (!load_module(target, ignored)) return false;
                }
            }

            ModuleUnit unit;
            unit.module_name = mod_name;
            unit.source_path = path;
            const std::string out_name = sinter::Semantic::to_snake_ident(mod_name);
            // 生成文件名加 sinter_ 前缀：避免与系统头（math.h / string.h / time.h …）
            // 或标准库同名文件冲突（-I <输出目录> 时 <math.h> 会被本地 math.h 遮蔽）。
            unit.output_c_path = output_dir_ / ("sinter_" + out_name + ".c");
            unit.output_h_path = output_dir_ / ("sinter_" + out_name + ".h");
            unit.program = std::move(prog);

            ordered_.push_back(std::move(unit));
            loaded_index_[key] = ordered_.size() - 1;
            return true;
        }
    };
}

int main(int argc, char *argv[]) {
    // `--version` / `--help` 必须在「把 argv[1] 当成 entry 路径」之前拦截：
    // 否则 `sinterc --version` 会变成 `entry not found: /abs/path/--version`。
    // 四个二进制（sinter / sinterc / sinterd / sinter-lsp）都支持这两个参数。
    auto print_usage = [] {
        std::cout <<
                "sinterc " SINTER_VERSION_STRING " — Sinter compiler (Sinter -> C)\n"
                "\n"
                "Usage: sinterc <entry.sinter> [-o <out_dir>] [-I <dir> ...]\n"
                "                [--emit-sources-list <file.cmake>]\n"
                "                [-D key=value ...]\n"
                "                [--cstd <std>]\n"
                "                [--target-os=<os>] [--target-arch=<arch>]\n"
                "                [--release|--debug]\n"
                "\n"
                "  --emit-sources-list <file>\n"
                "      Write the C sources produced by this run as a CMake-includable script,\n"
                "      so the build system gets the full source list at configure time (used by sinter).\n"
                "  --cstd <std>\n"
                "      C standard to print in the manual link command (e.g. c17 / gnu11).\n"
                "      It does not affect codegen; the actual flag lives in the build system.\n"
                "  --version, -V\n"
                "      Print the compiler version and exit.\n";
    };
    if (argc >= 2) {
        const std::string a1 = argv[1];
        if (a1 == "--version" || a1 == "-V") {
            std::cout << "sinterc " SINTER_VERSION_STRING "\n";
            return 0;
        }
        if (a1 == "--help" || a1 == "-h") {
            print_usage();
            return 0;
        }
    }
    if (argc < 2) {
        print_usage();
        return 1;
    }

    auto to_abs = [](const fs::path &p) -> fs::path {
        std::error_code ec;
        fs::path a = fs::absolute(p, ec);
        return ec ? p : a;
    };

    fs::path entry = to_abs(argv[1]);
    fs::path out_dir = ".";
    std::vector<fs::path> search_dirs;
    fs::path sources_list_path;
    /// 目标 C 标准（`--cstd`）：只用于打印那条「手动 gcc 链路命令」。
    /// 真正的编译标准由 sinter 写进 CMakeLists / 由用户自己决定。
    std::string c_std;
    sinter::ComptimeTarget target = sinter::host_target();
    std::vector<std::pair<std::string, std::string> > defines;

    // 修复参数解析：-o 与 -I 正确识别
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            out_dir = to_abs(argv[++i]);
        } else if (a == "-I" && i + 1 < argc) {
            search_dirs.push_back(to_abs(argv[++i]));
        } else if (a == "--emit-sources-list" && i + 1 < argc) {
            sources_list_path = to_abs(argv[++i]);
        } else if (a == "--cstd" && i + 1 < argc) {
            c_std = argv[++i];
        } else if (a.rfind("--cstd=", 0) == 0) {
            c_std = a.substr(7);
        } else if (a == "-D" && i + 1 < argc) {
            std::string kv = argv[++i];
            const auto eq = kv.find('=');
            if (eq == std::string::npos) defines.emplace_back(kv, "1");
            else defines.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        } else if (sinter::parse_target_flag(a, target)) {
            // 已由 parse_target_flag 写入 target
        } else if (a == "-h" || a == "--help") {
            std::cout << "Usage: sinterc <entry.sinter> [-o <out_dir>] [-I <dir> ...]\n"
                          "                [--emit-sources-list <file.cmake>]\n"
                          "                [-D key=value ...]\n"
                          "                [--cstd <std>]\n"
                          "                [--target-os=<os>] [--target-arch=<arch>]\n"
                          "                [--release|--debug]\n";
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option: " << a << "\n";
            return 1;
        } else {
            out_dir = to_abs(a);
        }
    }

    if (!fs::exists(entry)) {
        std::cerr << "entry not found: " << entry << "\n";
        return 1;
    }
    fs::create_directories(out_dir);

    ModuleLoader loader;
    for (auto &d: search_dirs) loader.add_search_dir(d);
    loader.set_output_dir(out_dir);

    // 泛型实例表：单态化填充，语义层与代码生成共用。
    sinter::GenericTable generic_table;

    std::cout << "Loading modules:\n";
    if (!loader.load_entry(entry)) {
        std::cerr << "load failed\n";
        return 1;
    }

    // ---------------------------------------------------------------------
    // 泛型类 / 泛型结构体单态化：在语义分析之前把 `Box<i64>` 退化成普通类
    // `Box_i64`。全部模块都已加载完成，因此跨模块使用也能正确定位到模板的
    // 声明模块，并在使用方补上 import。
    // ---------------------------------------------------------------------
    // 把 loader 里的模块转成「模块名 + AST + 源文件」——源文件是 __FILE__ 的值，
    // 只有 loader 知道（解析器只看得到源码文本）。
    const auto module_views = [&loader]() {
        std::vector<sinter::GenericModule> out;
        out.reserve(loader.modules().size());
        for (auto &m: loader.modules())
            out.push_back(sinter::GenericModule{m.module_name, &m.program, m.source_path.string()});
        return out;
    };

    {
        // 编译期特性：Target.* 折叠 + comptime if 条件裁剪。
        // 必须在泛型单态化之前，否则已裁掉的分支里的泛型也会被实例化。
        target.apply_defines(defines);
        auto ct_modules = module_views();
        std::string comptime_error;
        if (!sinter::apply_comptime(ct_modules, target, comptime_error)) {
            std::cerr << "compile-time evaluation failed: " << comptime_error << "\n";
            return 1;
        }
    }

    {
        auto generic_modules = module_views();
        std::string generic_error;
        if (!sinter::instantiate_generic_types(generic_modules, generic_error, &generic_table)) {
            std::cerr << "generic monomorphization failed: " << generic_error << "\n";
            return 1;
        }
    }

    {
        // 编译期位置关键字：__FILE__ / __MODULE__ / __LINE__ / __COLUMN__ /
        // __FUNCTION__ / __CLASS__ / __STRUCT__。
        // 放在单态化之后：实例体里的 __CLASS__ 应该是 `Box_i64` 而不是 `Box`。
        auto site_modules = module_views();
        std::string site_error;
        if (!sinter::apply_source_sites(site_modules, target, site_error)) {
            std::cerr << "compile-time evaluation failed: " << site_error << "\n";
            return 1;
        }
    }

    // 共享 Semantic
    sinter::Semantic sem;
    // 实例的类型实参名交给语义层：实例方法体里按 T 分派的多态调用
    // （`sinter_gen_clone(v)` 之类）要靠它解析出具体函数。
    {
        std::vector<std::string> inst_names;
        generic_table.names(inst_names);
        for (auto &n: inst_names) {
            std::vector<std::string> args;
            if (generic_table.split(n, args)) sem.declare_generic_instance(n, args);
        }
    }
    // 任何语义错误都必须中止编译（例如「函数冲突：请使用别名区分」）
    bool semantic_failed = false;

    // ---------------------------------------------------------------------
    // 跨模块继承：先为每个模块预登记它导出的类（名字 + C 结构名 + 构造参数），
    // 使后续模块 `class Child : Parent` 能解析出 Parent 的布局。
    // 这里只做一次轻量扫描，真正的完整登记发生在各自的 analyze() 里；
    // 由于 analyze 按依赖顺序进行，父模块总是先于子模块被分析，
    // 因此 external_classes_ 里能查到外来类的完整定义。
    // ---------------------------------------------------------------------
    auto preset_classes = [](sinter::Semantic &S, sinter::Program &prog,
                             const std::string &mod_name) {
        const std::string ident = S.module_to_ident(mod_name);
        std::function<void(sinter::Program &)> scan_pub;
        auto reg = [&](sinter::ClassDecl *c) {
            sinter::ClassInfo ci;
            ci.name = c->name;
            ci.c_name = ident + "_" + sinter::Semantic::to_snake_ident(c->name);
            ci.module_name = mod_name;
            ci.is_pub = c->is_pub;
            if (!c->bases.empty()) ci.base_class = c->bases[0];
            for (auto &prm: c->ctor_params) {
                sinter::ParamInfo pi;
                pi.name = prm.name;
                pi.c_name = sinter::Semantic::to_snake_ident(prm.name);
                pi.type_c = S.type_to_c(prm.type.get());
                pi.type_sinter = prm.type ? prm.type->name : "";
                ci.ctor_params.push_back(pi);
                sinter::FieldInfo fi;
                fi.name = prm.name;
                fi.c_name = sinter::Semantic::to_snake_ident(prm.name);
                fi.type_c = pi.type_c;
                fi.is_mutable = !prm.is_val;
                ci.fields.push_back(fi);
            }
            for (auto &m: c->members) {
                if (auto *v = dynamic_cast<sinter::VarDeclStmt *>(m.get())) {
                    sinter::FieldInfo fi;
                    fi.name = v->name;
                    fi.c_name = sinter::Semantic::to_snake_ident(v->name);
                    fi.is_static = v->is_static;
                    fi.is_mutable = v->is_mutable;
                    fi.type_c = S.type_to_c(v->type.get());
                    if (!fi.is_static) ci.fields.push_back(fi);
                }
            }
            S.declare_external_class(ci);
        };
        for (auto &st: prog.statements) {
            if (auto *c = dynamic_cast<sinter::ClassDecl *>(st.get())) {
                reg(c);
            } else if (auto *ms = dynamic_cast<sinter::ModuleStmt *>(st.get())) {
                for (auto &x: ms->body)
                    if (auto *c = dynamic_cast<sinter::ClassDecl *>(x.get())) reg(c);
            } else if (auto *pb = dynamic_cast<sinter::PubBlockStmt *>(st.get())) {
                for (auto &x: pb->statements)
                    if (auto *c = dynamic_cast<sinter::ClassDecl *>(x.get())) reg(c);
            }
        }
    };

    for (auto &m: loader.modules()) {
        try {
            if (std::getenv("SINTER_TRACE")) std::cerr << "[trace] preset " << m.module_name << "\n";
            preset_classes(sem, m.program, m.module_name);
            if (std::getenv("SINTER_TRACE")) std::cerr << "[trace] analyze " << m.module_name << "\n";
            // 诊断要定位到用户自己的源文件行，而不是模块标识符
            sem.set_current_file(m.source_path.string());
            sem.analyze(m.program, m.module_name);
            if (std::getenv("SINTER_TRACE")) std::cerr << "[trace] analyze done\n";
            // 类型检查：拦住会一路漏到 C 编译器的确定错误
            if (!sem.has_errors()) {
                sinter::TypeChecker typeck(sem);
                typeck.check_program(m.program);
                if (!sem.has_errors()) {
                    sinter::OwnershipChecker own(sem);
                    own.check_program(m.program);
                }
            }
            if (sem.has_errors()) semantic_failed = true;
        } catch (const std::exception &e) {
            std::cerr << "Semantic error in " << m.module_name << ": " << e.what() << "\n";
            return 1;
        }
    }
    // 警告/弱警告在成功时也要输出；以前只在 semantic_failed 分支里统一渲染，
    // 导致 `warning:` 永远看不到。
    for (const auto &d: sem.diagnostics())
        if (d.severity != sinter::Severity::Error) sinter::render_diagnostic(std::cerr, d);
    if (semantic_failed) {
        // 统一渲染：file:line:col: 错误: 消息 + 源码行 + ^^^ 指示
        for (const auto &d: sem.diagnostics())
            if (d.severity == sinter::Severity::Error) sinter::render_diagnostic(std::cerr, d);
        std::cerr << "semantic analysis failed; code generation skipped.\n";
        return 1;
    }

    // 运行时支持库：独立生成 sinter_runtime.h / sinter_runtime.c（一次编译只产生一份）
    {
        std::ofstream rt_h(out_dir / "sinter_runtime.h");
        std::ofstream rt_c(out_dir / "sinter_runtime.c");
        if (!rt_h || !rt_c) {
            std::cerr << "cannot write runtime files into " << out_dir << "\n";
            return 1;
        }
        rt_h << sinter::Codegen::runtime_header();
        rt_c << sinter::Codegen::runtime_source();
    }

    // 跨模块泛型函数实例化：先全局扫描所有模块的调用点，按「定义模块」归组。
    // 这样 std.memory 里的 `fun load<T>(p: *T)` 被用户模块调用时，定义模块
    // 也会生成 `load_i64` 的原型与实现。
    std::unordered_map<std::string, sinter::Codegen::GenericFunctionInstantiations>
        generic_functions_by_module;
    for (auto &m: loader.modules()) {
        sinter::Codegen collector(sem);
        collector.set_generic_table(&generic_table);
        sinter::Codegen::GenericFunctionInstantiations insts;
        collector.collect_generic_instantiations(m.program, insts);
        for (auto &[decl, substs]: insts) {
            std::string owner;
            for (auto &kv: sem.functions()) {
                for (auto &fi: kv.second) {
                    if (fi.decl == decl) { owner = fi.module_name; break; }
                }
                if (!owner.empty()) break;
            }
            if (owner.empty()) continue;
            auto &dst = generic_functions_by_module[owner][decl];
            for (auto &sub: substs) {
                bool seen = false;
                for (auto &x: dst) if (x == sub) { seen = true; break; }
                if (!seen) dst.push_back(sub);
            }
        }
    }

    // 生成 .c / .h
    for (auto &m: loader.modules()) {
        try {
            sinter::Codegen cg_c(sem);
            cg_c.set_generic_table(&generic_table);
            auto gfi = generic_functions_by_module.find(m.module_name);
            if (gfi != generic_functions_by_module.end())
                cg_c.set_generic_function_instantiations(&gfi->second);
            std::string c_code = cg_c.generate_c(m.program, m.module_name);

            sinter::Codegen cg_h(sem);
            cg_h.set_generic_table(&generic_table);
            if (gfi != generic_functions_by_module.end())
                cg_h.set_generic_function_instantiations(&gfi->second);
            std::string h_code = cg_h.generate_h(m.program, m.module_name);

            std::ofstream out_c(m.output_c_path);
            std::ofstream out_h(m.output_h_path);
            if (!out_c || !out_h) {
                std::cerr << "cannot write " << m.output_c_path << "\n";
                return 1;
            }
            out_c << c_code;
            // 入口模块：生成 C 的 main()，转调 Sinter 侧 main()
            if (m.source_path == entry) {
                auto it = sem.functions().find("main");
                if (it != sem.functions().end() && !it->second.empty()) {
                    const auto &mf = it->second.front();
                    const bool returns_void = mf.return_type_name.empty() || mf.return_type_name == "void";
                    // 与 C 一致：入口可以写 `fun main()`、`fun main(argc: i32)` 或
                    // `fun main(argc: i32, argv: **u8): i32`。后两种直接把宿主
                    // argc/argv 交出去，返回值就是进程退出码。
                    // 入口只支持 C 风格：`fun main()` / `fun main(argc: i32)` /
                    // `fun main(argc: i32, argv: **u8 | **char): i32`。
                    // 需要字符串数组时用指针操作自行转换（Sinter 支持 `*p` / `&x`），
                    // 编译器不再内置 `List<str>` 形态。
                    out_c << "\nint main(int argc, char** argv) {\n";
                    out_c << "    (void)argc;\n    (void)argv;\n";
                    if (mf.params.empty()) {
                        out_c << "    " << mf.c_name << "();\n";
                        out_c << "    return 0;\n";
                    } else if (mf.params.size() == 1) {
                        out_c << "    " << (returns_void ? "" : "int __rc = ")
                              << mf.c_name << "((int32_t)argc);\n";
                        out_c << (returns_void ? "    return 0;\n" : "    return (int)__rc;\n");
                    } else {
                        // 用声明里的第二形参类型做转换，别硬编码 uint8_t**：
                        // `**u8` / `**char` / 其它指针写法都要能编过。
                        const std::string argv_t =
                                mf.params.size() > 1 && !mf.params[1].type_c.empty()
                                        ? mf.params[1].type_c
                                        : std::string("uint8_t**");
                        out_c << "    " << (returns_void ? "" : "int __rc = ")
                              << mf.c_name << "((int32_t)argc, (" << argv_t << ")argv);\n";
                        out_c << (returns_void ? "    return 0;\n" : "    return (int)__rc;\n");
                    }
                    out_c << "}\n";
                }
            }
            out_h << h_code;
            // 代码生成阶段也可能报告错误（如数组元素类型暂不支持析构）。
            // 一旦有错误就不能把半成品当成可用的 C 源码交出去。
            if (sem.has_errors()) {
                for (const auto &d: sem.diagnostics()) sinter::render_diagnostic(std::cerr, d);
                std::cerr << "code generation failed; no output written.\n";
                return 1;
            }
            std::cout << "  [module] " << m.module_name
                      << " -> " << m.output_c_path.string()
                      << " + " << m.output_h_path.string() << "\n";
        } catch (const std::exception &e) {
            std::cerr << "Codegen error in " << m.module_name << ": " << e.what() << "\n";
            return 1;
        }
    }

    std::cout << "\nLink command:\n  gcc ";
    if (!c_std.empty()) std::cout << "-std=" << c_std << " ";
    std::cout << (out_dir / "sinter_runtime.c").string() << " ";
    for (auto &m: loader.modules())
        std::cout << m.output_c_path.string() << " ";
    std::cout << "-o " << (out_dir / "program").string()
              << " -I " << out_dir.string() << " -lm";
#ifndef _WIN32
    // std.thread / std.async 需要 pthread；glibc 2.34+ 已并入 libc，但老系统仍要显式链接。
    std::cout << " -pthread";
#endif
    std::cout << "\n";

    // --emit-sources-list：把这次产出的全部 .c 写成一份 CMake include 文件。
    //
    // 为什么需要：sinterc 一次调用产出「入口模块 + 全部被 import 的模块 + runtime」，
    // 具体有哪几个要到运行时才知道，而 CMake 在配置期就必须知道要编哪些源文件。
    // 由编译器自己写出清单，构建系统直接读，避免一边用通配符、一边漏文件。
    if (!sources_list_path.empty()) {
        std::ofstream out(sources_list_path);
        if (!out) {
            std::cerr << "cannot write sources list: " << sources_list_path << "\n";
            return 1;
        }
        out << "# Generated by sinterc: C sources produced by this run (for CMake include)\n";
        out << "set(SINTER_SOURCE_LIST\n";
        // runtime 必须在内核模块之前无关紧要，但保持稳定顺序便于 diff
        out << "        \"" << (out_dir / "sinter_runtime.c").string() << "\"\n";
        for (auto &m: loader.modules())
            out << "        \"" << m.output_c_path.string() << "\"\n";
        out << ")\n";
    }

    return 0;
}