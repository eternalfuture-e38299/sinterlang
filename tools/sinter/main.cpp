// sinter —— sinter package and build 
//
// buildpath：CMake。
//   sinter build      -> generate CMake projectto  build/， call cmake  + build
//   sinter emit-cmake -> generate CMake project，not build
//  cmake_gen   generate，so「cannotcompile」 CMake a ，
// not will 「sinter build  、 CMake buildnot  」。
#include "bundled.hpp"
#include "cmake_gen.hpp"
#include "manifest.hpp"
#include "registry.hpp"

#include <sinter/version.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace sinter;

namespace {
    /// 版本号来自 CMake 的 project(VERSION)：sinter / sinterc / sinterd / sinter-lsp 共用。
    constexpr const char *kVersion = SINTER_VERSION_STRING;

    void usage() {
        std::cout <<
                "sinter " << kVersion << " — Sinter package manager\n"
                "\n"
                "usage: sinter <command> [args]\n"
                "\n"
                "project commands:\n"
                "  init [name]                 create sinter.toml and src/main.sinter\n"
                "  add <package>[@version]     add a dependency and write it into sinter.toml\n"
                "  remove <package>            remove a dependency\n"
                "  install                     resolve dependencies and write sinter.lock\n"
                "  build [--run] [--debug|--release] [--opt <0-3|s|z>] [-j N] [--cstd <std>]\n"
                "        [--toolchain <file>]\n"
                "        [--cmake-arg <arg> ...]\n"
                "                              build with CMake (--run executes the artifact);\n"
                "                              --toolchain passes CMake toolchain file (cross builds);\n"
                "                              --cmake-arg forwards a -D.../option to CMake configure\n"
                "  emit-cmake [dir] [--cstd <std>]\n"
                "                              generate the CMake project only, do not build\n"
                "  emit-c [dir] [--debug] [--cstd <std>]\n"
                "                              generate C sources + CMake project, do not call cmake\n"
                "                              (for hosts without cmake, e.g. Android)\n"
                "  tree                        print the dependency tree\n"
                "  info                        print the resolved manifest summary\n"
                "  clean                       remove build/ (keeps sinter.lock)\n"
                "\n"
                "registry commands:\n"
                "  publish [--path <dir>]      upload a package to the registry\n"
                "  list                        list packages in the registry\n"
                "  search <keyword>            search the registry\n"
                "\n"
                "environment:\n"
                "  SINTER_HOME                 registry directory (default ~/.sinter)\n"
                "  SINTERC                     path to sinterc (default: found in PATH)\n"
                "  SINTER_BUNDLED_DIR          bundled packages directory\n"
                "  CMAKE                       cmake executable (default: found in PATH)\n"
                "  SINTER_CSTD                 default C standard, overrides sinter.toml\n"
                "\n"
                "C standard (--cstd, SINTER_CSTD, or [build] cstd in sinter.toml):\n"
                "  c99 / c11 / c17 / c23       strict ISO C\n"
                "  gnu99 / gnu11 / gnu17 / gnu23\n"
                "                              ISO C plus GNU extensions (default: c99)\n"
                "  C99 is the minimum — the generated C uses for-init and mixed declarations.\n"
                "\n"
                "manifest keys ([package] in sinter.toml):\n"
                "  kind = \"sinter\"             Sinter source package (default), built by sinterc\n"
                "  kind = \"cmake\"              the package is a CMake project\n"
                "\n"
                "artifact keys (what the package produces):\n"
                "  artifact = \"exe\"            executable (default for Sinter packages)\n"
                "  artifact = \"static\"         static library (default for CMake packages)\n"
                "  artifact = \"shared\"         shared library\n"
                "  artifact = \"header\"         headers only, no binary\n"
                "  Executable packages cannot be used as dependencies; a library package\n"
                "  must declare target = \"<exported CMake target>\".\n";
    }

    /// 写文件，**比较内容而不是 mtime**。
    ///
    /// CMake 只在文件内容变化时才需要重新配置；每个 `sinter build` 都重写一遍
    /// CMakeLists.txt / 包清单却不比较内容，会让缓存反复失效（每次都重新编译）。
    bool write_file(const fs::path &p, const std::string &text) {

        std::error_code ec;
        if (fs::exists(p, ec)) {
            if (std::ifstream in(p, std::ios::binary); in) {
                std::ostringstream ss;
                ss << in.rdbuf();
                if (ss.str() == text) return true;   // already   ， mtime
            }
        }
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p);
        if (!f) return false;
        f << text;
        return true;
    }

    std::string env_or(const char *k, const std::string &d) {
        const char *v = std::getenv(k);
        return (v && *v) ? std::string(v) : d;
    }

    /// linecommand（argsarray，not  shell string）。
    ///
    ///  std::system(cmd.c_str()) command，path 、 、
    /// `$`  or  `;`  will  shell then  ——failed，  command。
    ///   fork/execvp  args，not  shell parse。
    int run_argv(const std::vector<std::string> &argv, const bool echo) {
        if (argv.empty()) return 1;
        if (echo) {
            std::cout << "  $";
            for (const auto &a: argv) std::cout << " " << a;
            std::cout << "\n";
        }
        std::cout.flush();

        std::vector<char *> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto &a: argv) cargv.push_back(const_cast<char *>(a.c_str()));
        cargv.push_back(nullptr);

        const pid_t pid = ::fork();
        if (pid < 0) {
            std::cerr << "cannot create process\n";
            return 1;
        }
        if (pid == 0) {
            ::execvp(cargv[0], cargv.data());
            std::cerr << "cannot execute " << cargv[0] << "\n";
            ::_exit(127);
        }
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) return 1;
        }
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return 1;
    }

    // line： line packageregistry
    void ensure_bundled() {
        if (std::getenv("SINTER_NO_BUNDLED")) return;
        std::string err;
        install_bundled(err);
    }

    /// parsedependencyand error。failedreturn  false（not dependency）。
    bool resolve_or_report(const Manifest &m, const fs::path &cwd,
                           std::vector<ResolvedPackage> &deps, const char *what) {
        ensure_bundled();
        Registry reg;
        std::string err;
        if (!reg.load(err)) {
            std::cerr << err << "\n";
            return false;
        }
        if (!resolve_dependencies(m, cwd.string(), reg, deps, err)) {
            std::cerr << what << "failed: " << err << "\n";
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------- command
    int cmd_init(const std::string &name) {
        const fs::path cwd = fs::current_path();
        if (fs::exists(cwd / "sinter.toml")) {
            std::cerr << "sinter.toml already exists\n";
            return 1;
        }
        Manifest m;
        m.name = name.empty() ? cwd.filename().string() : name;
        if (m.name.empty()) m.name = "app";
        m.description = "";
        m.license = "MIT";
        // code  `import std`，somanifestmustdeclarationdependency。
        // version， init  projectfirst build 
        // 「import not found: std」—— std its line 。
        Dependency std_dep;
        std_dep.name = "std";
        std_dep.version = "*";
        m.dependencies.push_back(std_dep);
        std::string err;
        if (!m.save((cwd / "sinter.toml").string(), err)) {
            std::cerr << err << "\n";
            return 1;
        }
        write_file(cwd / "src" / "main.sinter",
                   "module " + m.name + "\n"
                   "\n"
                   "import std\n"
                   "\n"
                   "fun main(): void {\n"
                   "    println(\"hello, sinter\")\n"
                   "}\n");
        std::cout << "created project " << m.name << "\n"
                  << "  sinter.toml\n"
                  << "  src/main.sinter\n"
                  << "\nbuild: sinter build --run\n";
        return 0;
    }

    int cmd_add(const std::string &spec) {
        const fs::path cwd = fs::current_path();
        Manifest m;
        std::string err;
        if (!Manifest::load((cwd / "sinter.toml").string(), m, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        std::string name = spec, version = "*";
        if (const auto at = spec.find('@'); at != std::string::npos) {
            name = spec.substr(0, at);
            version = spec.substr(at + 1);
        }
        // pathdependency：sinter add ./libs/foo
        Dependency d;
        d.name = name;
        d.version = version;
        std::error_code ec;
        if (fs::exists(spec, ec) && fs::is_directory(spec, ec)) {
            d.source = "path:" + fs::absolute(spec).string();
            Manifest dm;
            if (Manifest::load((fs::path(spec) / "sinter.toml").string(), dm, err)) {
                d.name = dm.name;
                d.version = dm.version;
                const Artifact a = dm.artifact_explicit
                                       ? dm.artifact
                                       : (dm.is_cmake_package() ? Artifact::StaticLib
                                                                : Artifact::Executable);
                if (a == Artifact::Executable) {
                    // linepackagecannot dependency， etc resolve  
                    std::cerr << dm.name << "   artifact = \"exe\" (executable), "
                                 "cannot its packagedependency\n";
                    return 1;
                }
                if (dm.is_cmake_package() && a != Artifact::HeaderOnly && !dm.target.empty()) {
                    // packagemust target，then not link
                    d.target = dm.target;
                }
                std::cout << "detected as " << (dm.is_cmake_package() ? "CMake project" : "Sinter source")
                          << "package，artifact " << artifact_name(a);
                if (!d.target.empty()) std::cout << ", exported target " << d.target;
                std::cout << "\n";
            }
        } else {
            d.source = "registry";
        }
        m.dependencies.erase(std::remove_if(m.dependencies.begin(), m.dependencies.end(),
                                            [&](const Dependency &x) { return x.name == d.name; }),
                             m.dependencies.end());
        m.dependencies.push_back(d);
        if (!m.save((cwd / "sinter.toml").string(), err)) {
            std::cerr << err << "\n";
            return 1;
        }
        std::cout << "added dependency " << d.name << " " << d.version << "\n";
        return 0;
    }

    int cmd_remove(const std::string &name) {
        const fs::path cwd = fs::current_path();
        Manifest m;
        std::string err;
        if (!Manifest::load((cwd / "sinter.toml").string(), m, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        const auto before = m.dependencies.size();
        m.dependencies.erase(std::remove_if(m.dependencies.begin(), m.dependencies.end(),
                                            [&](const Dependency &x) { return x.name == name; }),
                             m.dependencies.end());
        if (m.dependencies.size() == before) {
            std::cerr << "dependency no  " << name << "\n";
            return 1;
        }
        if (!m.save((cwd / "sinter.toml").string(), err)) {
            std::cerr << err << "\n";
            return 1;
        }
        std::cout << "removed dependency " << name << "\n";
        return 0;
    }

    int cmd_install() {
        const fs::path cwd = fs::current_path();
        Manifest m;
        std::string err;
        if (!Manifest::load((cwd / "sinter.toml").string(), m, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        std::vector<ResolvedPackage> deps;
        if (!resolve_or_report(m, cwd, deps, "dependencies")) return 1;

        // file：parseresult（packagetype and artifact，）
        std::ostringstream lock;
        lock << "#  sinter install generate，\n";
        lock << "version = 3\n";
        for (const auto &d: deps) {
            lock << "\n[[package]]\n";
            lock << "name = \"" << d.name << "\"\n";
            lock << "version = \"" << d.version << "\"\n";
            lock << "kind = \"" << package_kind_name(d.kind) << "\"\n";
            lock << "artifact = \"" << artifact_name(d.artifact) << "\"\n";
            if (!d.target.empty()) lock << "target = \"" << d.target << "\"\n";
            lock << "path = \"" << d.path << "\"\n";
        }
        write_file(cwd / "sinter.lock", lock.str());
        if (deps.empty()) std::cout << "no dependencies to install\n";
        else {
            std::cout << "installed " << deps.size() << "  dependency:\n";
            for (const auto &d: deps)
                std::cout << "  " << d.name << " " << d.version << "  ["
                          << artifact_name(d.artifact) << "]"
                          << (d.target.empty() ? std::string() : "  target " + d.target)
                          << "  （" << d.path << "）\n";
        }
        return 0;
    }

    // ---------------------------------------------------------------- build
    ///  generate projectto （CMakeLists.txt）
    /// @param src_root projectdirectory（ sinter.toml，parse entry/src）
    /// @param dst      projectfilewritedirectory（builddirectory）
    bool write_project(const fs::path &src_root, const fs::path &dst, const Manifest &m,
                       const std::vector<ResolvedPackage> &deps,
                       const CMakeGenOptions &opt, CMakeGenResult &res) {
        // note：src_root  and  dst must。dst   build/cmake，src_root  
        //  src/main.sinter  projectdirectory；version ，generate 
        // SINTER_ENTRY will  build/cmake/src/main.sinter（not  ）。
        res = generate_cmakelists(src_root.string(), dst.string(), m, deps, opt);
        if (!write_file(dst / "CMakeLists.txt", res.cmakelists)) {
            std::cerr << "cannot write " << (dst / "CMakeLists.txt").string() << "\n";
            return false;
        }
        return true;
    }

    /// 目标 C 标准：`--cstd` > 环境变量 `SINTER_CSTD` > `sinter.toml [build] cstd` > 默认 c99。
    /// 解析失败时把原因打到 stderr 并返回 false。
    bool resolve_c_std(const Manifest &m, const std::string &cli, CStandard &out) {
        std::string text = cli;
        if (text.empty()) text = env_or("SINTER_CSTD", "");
        if (text.empty()) text = m.c_std;
        std::string err;
        if (!CStandard::parse(text, out, err)) {
            std::cerr << "cstd: " << err << "\n";
            return false;
        }
        return true;
    }

    /// 把最终生效的标准打出来：构建日志里能对上 cmake 里那行 set(CMAKE_C_STANDARD …)。
    void report_c_std(const CStandard &cs) {
        std::cout << "C standard: " << cs.name << " (CMAKE_C_STANDARD " << cs.cmake_version
                  << ", extensions " << (cs.extensions ? "ON" : "OFF") << ")\n";
    }

    int cmd_build(const bool do_run, const bool debug, const int jobs, const std::string &cstd_cli,
                  const std::string &opt_cli = {},
                  const std::string &toolchain = {},
                  const std::vector<std::string> &cmake_args = {}) {
        const fs::path cwd = fs::current_path();
        Manifest m;
        std::string err;
        if (!Manifest::load((cwd / "sinter.toml").string(), m, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        // condition：CMake  packagethe  cmake build
        // （Sinter source packagecan build，is artifact sinterc generate）
        if (m.is_cmake_package() && m.artifact != Artifact::HeaderOnly) {
            std::cerr << " a  CMake projectpackage（kind = \"cmake\"): its sources are built by its own "
                         "CMakeLists.txt 。\n"
                         "build，please  CMake：\n"
                      << "  cmake -S \"" << (cwd / m.cmake_dir).string() << "\" -B build\n"
                      << "  cmake --build build\n"
                      << "is dependency  projectreference ，will  sinter 。\n";
            return 1;
        }

        std::vector<ResolvedPackage> deps;
        if (!resolve_or_report(m, cwd, deps, "dependencies")) return 1;

        CStandard cstd;
        if (!resolve_c_std(m, cstd_cli, cstd)) return 1;
        report_c_std(cstd);

        // CMake packagecheck： CMakeLists.txt /  target  ，
        //  and its etc cmake   FATAL_ERROR，not  description。
        bool bad = false;
        for (const auto &d: deps) {
            if (!d.is_cmake()) continue;
            const std::string why = check_cmake_package(d);
            if (!why.empty()) {
                std::cerr << "dependencynot : " << why << "\n";
                bad = true;
            }
        }
        if (bad) return 1;

        const fs::path out = cwd / "build";
        // CMakeLists.txt   build/ ， builddirectory cmake/：
        // directorywill  CMakeLists.txt  and  CMakeCache.txt  ， 。
        const fs::path proj_dir = out / "cmake";
        std::error_code ec;
        fs::create_directories(proj_dir, ec);

        CMakeGenOptions opt;
        opt.project_name = m.name;
        opt.sinterc = env_or("SINTERC", "");
        opt.c_standard = cstd.cmake_version;
        opt.c_extensions = cstd.extensions;
        // artifact：manifest declaration ，then  kind default
        opt.artifact = m.artifact_explicit
                           ? m.artifact
                           : (m.is_cmake_package() ? Artifact::StaticLib : Artifact::Executable);
        // package exported target（Sinter source <name>_sinter， target   target）
        opt.target_name = m.target;
        CMakeGenResult gen;
        if (!write_project(cwd, proj_dir, m, deps, opt, gen)) return 1;

        const auto cmake_deps = cmake_dependencies(deps);
        if (!cmake_deps.empty()) {
            std::cout << "dependency:\n";
            for (const auto *d: cmake_deps)
                std::cout << "  " << d->name << " " << d->version << "  ["
                          << artifact_name(d->artifact) << "]"
                          << (d->target.empty() ? std::string() : "  -> target " + d->target)
                          << "\n";
        }

        // ---- cmake  ----
        // note：-D must args "-DKEY=VALUE"， will   CMake 
        //  positionargs（ "Ignoring extra path from command line"）。
        const std::string cmake_exe = env_or("CMAKE", "cmake");
        std::string opt_level = opt_cli.empty() ? (m.opt.empty() ? env_or("SINTER_OPT", "") : m.opt) : opt_cli;
        std::vector<std::string> cfg{cmake_exe, "-S", proj_dir.string(), "-B", proj_dir.string(),
                                     std::string("-DCMAKE_BUILD_TYPE=") +
                                             (debug ? "Debug" : "Release")};
        if (!debug && !opt_level.empty()) {
            std::string flag = "-O2";
            if (opt_level == "0") flag = "-O0";
            else if (opt_level == "1") flag = "-O1";
            else if (opt_level == "3") flag = "-O3";
            else if (opt_level == "s" || opt_level == "S") flag = "-Os";
            else if (opt_level == "z" || opt_level == "Z") flag = "-Oz";
            else if (opt_level == "g") flag = "-Og";
            else if (opt_level.rfind("-O", 0) == 0) flag = opt_level;
            cfg.push_back("-DCMAKE_C_FLAGS_RELEASE=" + flag + " -DNDEBUG");
        }
        //  sinterc position，generateproject  PATH  not to compile
        const std::string sinterc = env_or("SINTERC", "");
        if (!sinterc.empty()) cfg.push_back("-DSINTERC_EXE=" + sinterc);
        // 跨平台：传 CMake toolchain 文件（android/linux/windows/macos 都可以）。
        // 显式指定时强制重新配置，避免复用上一次宿主编译的 cache。
        if (!toolchain.empty()) cfg.push_back("-DCMAKE_TOOLCHAIN_FILE=" + toolchain);
        for (const auto &arg: cmake_args) cfg.push_back(arg);

        // failed failed，not requires「」：generate  CMakeLists  
        // will  execute_process  sinterc to filemanifest（ cmake_gen.cpp）。
        // version ，failed will heap
        // "No rule to make target 'Makefile'"， error。
// cache：CMakeCache.txt  and not  CMakeLists.txt  「」，
        //  build（CMake  will check，each  execute_process
        //  sinterc filemanifest，cachecompile ）。
        const fs::path cache_file = proj_dir / "CMakeCache.txt";
        const fs::path cmake_lists = proj_dir / "CMakeLists.txt";
        const fs::path sources_list = proj_dir / "gen" / "sources.cmake";
        std::error_code tec;
        bool need_configure = true;
        if (!toolchain.empty() || !cmake_args.empty()) {
            if (!toolchain.empty()) std::cout << "  (toolchain: reconfiguring CMake)\n";
        } else if (fs::exists(cache_file, tec) && fs::exists(cmake_lists, tec) &&
                   fs::exists(sources_list, tec)) {
            const auto cache_t = fs::last_write_time(cache_file, tec);
            const auto lists_t = fs::last_write_time(cmake_lists, tec);
            if (!tec && cache_t >= lists_t) need_configure = false;
        }
        // 缓存里记着**上一次**的 sinterc 路径。IDE 切换工具链 / 重建 sinterc 后
        // 路径变了，复用旧 cache 会让配置期跑旧二进制（甚至报“文件不存在”），
        // 表现为「第一次能编，后面再也编不了」。路径不一致就强制重新配置。
        if (!need_configure && !sinterc.empty()) {
            std::ifstream cache_in(cache_file);
            std::string line;
            while (std::getline(cache_in, line)) {
                if (line.rfind("SINTERC_EXE:", 0) != 0) continue;
                const auto eq = line.find('=');
                if (eq != std::string::npos && line.substr(eq + 1) != sinterc) {
                    need_configure = true;
                    std::cout << "  (sinterc path changed; reconfiguring CMake)\n";
                }
                break;
            }
        }
        // CMakeCache 里写死了创建时的源码目录与构建目录。工程被复制/移动后
        // （Sinter Studio 导入、打包到另一台机器、重命名目录）继续用这份 cache
        // 会让 cmake 直接报 "current CMakeCache.txt directory ... is different"，
        // 而且**每次**都失败。发现路径不匹配就先删掉，让它从头配置。
        if (fs::exists(cache_file, tec)) {
            std::ifstream cache_in(cache_file);
            std::string line;
            bool stale_paths = false;
            while (std::getline(cache_in, line)) {
                auto value_of = [&](const char *prefix) -> std::string {
                    const std::string p(prefix);
                    if (line.rfind(p, 0) != 0) return {};
                    return line.substr(p.size());
                };
                std::string value = value_of("CMAKE_HOME_DIRECTORY:INTERNAL=");
                if (value.empty()) value = value_of("CMAKE_CACHEFILE_DIR:INTERNAL=");
                if (value.empty()) continue;
                std::error_code pec;
                if (fs::weakly_canonical(value, pec) != fs::weakly_canonical(proj_dir, pec))
                    stale_paths = true;
            }
            if (stale_paths) {
                std::error_code rec;
                fs::remove(cache_file, rec);
                fs::remove_all(proj_dir / "CMakeFiles", rec);
                need_configure = true;
                std::cout << "  (project moved; discarding stale CMake cache)\n";
            }
        }
        if (need_configure) {
            if (run_argv(cfg, true) != 0) {
                // 配置失败时清掉 cache，避免下一次继续复用半成品状态。
                std::error_code rec;
                fs::remove(cache_file, rec);
                std::cerr << "CMake configure failed\n";
                return 1;
            }
        } else {
            std::cout << "  (reusing CMake cache: " << cache_file.string() << ")\n";
        }

        // ---- cmake build ----
        std::vector<std::string> bld{cmake_exe, "--build", proj_dir.string()};
        if (jobs > 0) {
            bld.push_back("-j");
            bld.push_back(std::to_string(jobs));
        }
        //   gen/*.c notexists，CMake to，linkfailed；
        // CONFIGURE_DEPENDS  build  glob，soi.e.。
        int build_rc = run_argv(bld, true);
        if (build_rc != 0 && need_configure) build_rc = run_argv(bld, true);
        if (build_rc != 0) {
            // 失败后清除 cache：下次一定重新配置，不会卡在「同一份坏缓存」上。
            std::error_code rec;
            fs::remove(cache_file, rec);
            std::cerr << "build failed（已清除 CMakeCache，下次构建会重新配置）\n";
            return 1;
        }
        //   gen/*.c notexists，CMake to；CONFIGURE_DEPENDS
        //  build ， compile。

        // ---- validateartifact ----
        // line and  artifactpathnot ；filepackagenot binary，check。
        const Artifact art = opt.artifact;
        if (art == Artifact::HeaderOnly) {
            std::cout << "header-only package, no binary produced\n";
            return 0;
        }
        fs::path bin = proj_dir / gen.artifact_path;
        // Cross builds for Windows add `.exe`; accept it without needing target sniffing.
        if (!fs::exists(bin, ec) && art == Artifact::Executable) {
            fs::path exe = bin;
            exe += ".exe";
            if (fs::exists(exe, ec)) bin = exe;
        }
        if (!fs::exists(bin, ec)) {
            std::cerr << "build finished but artifact not found: " << bin.string() << "\n";
            return 1;
        }
        std::cout << "generated " << bin.string() << " (" << artifact_name(art) << ")\n";
        if (do_run) {
            if (art != Artifact::Executable) {
                std::cerr << "artifact \"" << artifact_name(art)
                          << "\" is not executable; --run needs an executable artifact\n";
                return 1;
            }
            std::cout << "---- run ----\n";
            return run_argv({bin.string()}, false);
        }
        return 0;
    }

    int cmd_emit_cmake(const std::string &outdir, const std::string &cstd_cli) {
        const fs::path cwd = fs::current_path();
        Manifest m;
        std::string err;
        if (!Manifest::load((cwd / "sinter.toml").string(), m, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        std::vector<ResolvedPackage> deps;
        if (!resolve_or_report(m, cwd, deps, "dependencies")) return 1;

        CStandard cstd;
        if (!resolve_c_std(m, cstd_cli, cstd)) return 1;
        report_c_std(cstd);

        // defaultoutputto  build/， and  sinter build  projectposition，
        //   CMakeLists.txt project。
        const fs::path dst = outdir.empty() ? cwd / "build" : fs::path(outdir);
        std::error_code ec;
        fs::create_directories(dst, ec);

        CMakeGenOptions opt;
        opt.project_name = m.name;
        opt.c_standard = cstd.cmake_version;
        opt.c_extensions = cstd.extensions;
        // artifact：declaration，then  kind default
        opt.artifact = m.artifact_explicit
                           ? m.artifact
                           : (m.is_cmake_package() ? Artifact::StaticLib : Artifact::Executable);
        opt.target_name = m.target;
        CMakeGenResult gen;
        if (!write_project(cwd, dst, m, deps, opt, gen)) return 1;

        for (const auto &w: gen.warnings) std::cerr << "warning: " << w << "\n";

        std::cout << "generated CMake project: " << (dst / "CMakeLists.txt").string() << "\n"
                  << "artifact: " << artifact_name(opt.artifact) << "\n";
        if (!gen.targets.empty()) {
            std::cout << "linked dependency targets:\n";
            for (const auto &t: gen.targets) std::cout << "  " << t << "\n";
        }
        // note -S must  CMakeLists.txt  directory（dst），not  project：
        // version  cwd， emit-cmake   cmake not to project。
        const fs::path build_dir = dst / "cmake";
        std::cout << "\nbuild:\n"
                  << "  cmake -S \"" << dst.string() << "\" -B \"" << build_dir.string()
                  << "\" -DCMAKE_BUILD_TYPE=Release\n"
                  << "  cmake --build \"" << build_dir.string() << "\" -j\n";
        if (!gen.artifact_path.empty())
            std::cout << "artifact: " << (build_dir / gen.artifact_path).string() << "\n";
        std::cout << "\nhint: will line sinterc generatefilemanifest（gen/sources.cmake），\n"
                  << "      sorequires cmake i.e. ，not 。\n";
        return 0;
    }

    /// `sinter emit-c [directory] [--debug]`：「dependencies + sinterc generate C」，not call cmake。
    ///
    ///   ：`sinter build`     CMake， CMake 
    /// x86_64 / darwin package——Android no  cmake executable。
    ///  「Sinter -> C」，codegenerate：
    ///   *  `build/gen/*.c` + `build/gen/sources.cmake`（sinterc ）；
    ///   *   `build/CMakeLists.txt`，to a directory
    ///     `cmake -S build -B build/cmake` compile，not  sinter。
    ///
    ///   Android  defaultbuild（ sinter-studio   SinterBuildService），
    ///   「no  cc  a artifact」 。
    int cmd_emit_c(const std::string &outdir, bool debug, const std::string &cstd_cli) {
        const fs::path cwd = fs::current_path();
        Manifest m;
        std::string err;
        if (!Manifest::load((cwd / "sinter.toml").string(), m, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        CStandard cstd;
        if (!resolve_c_std(m, cstd_cli, cstd)) return 1;
        report_c_std(cstd);
        const Artifact art = m.artifact_explicit
                                 ? m.artifact
                                 : (m.is_cmake_package() ? Artifact::StaticLib : Artifact::Executable);
        if (m.is_cmake_package() && art != Artifact::HeaderOnly) {
            std::cerr << " a  CMake projectpackage（kind = \"cmake\"），no Sinter sources to compile。\n";
            return 1;
        }

        std::vector<ResolvedPackage> deps;
        if (!resolve_or_report(m, cwd, deps, "dependencies")) return 1;

        const fs::path dst = outdir.empty() ? cwd / "build" : fs::path(outdir);
        std::error_code ec;
        fs::create_directories(dst, ec);
        if (!fs::exists(dst, ec)) {
            std::cerr << "cannot create output directory " << dst.string() << "\n";
            return 1;
        }

        // CMakeLists.txt ：generate project  cmake 。
        CMakeGenOptions opt;
        opt.project_name = m.name;
        opt.sinterc = env_or("SINTERC", "");
        opt.c_standard = cstd.cmake_version;
        opt.c_extensions = cstd.extensions;
        opt.artifact = art;
        opt.target_name = m.target;
        CMakeGenResult gen;
        if (!write_project(cwd, dst, m, deps, opt, gen)) return 1;
        for (const auto &w: gen.warnings) std::cerr << "warning: " << w << "\n";

        if (art == Artifact::HeaderOnly) {
            std::cout << "header-only package, no codegen needed\n"
                      << "generated CMake project: " << (dst / "CMakeLists.txt").string() << "\n";
            return 0;
        }

        //  and generate  CMakeLists.txt   sinterc call：
        //   <sinterc> <entry> -o <gen> --emit-sources-list <gen>/sources.cmake -I <src> -I <dep> ...
        // ，"、not " will ，soargs。
        std::string sinterc = env_or("SINTERC", "");
        if (sinterc.empty()) sinterc = "sinterc";

        const fs::path gen_dir = dst / "gen";
        fs::create_directories(gen_dir, ec);

        std::vector<std::string> cmd{sinterc, (cwd / m.entry).string(), "-o", gen_dir.string(),
                                     "--emit-sources-list", (gen_dir / "sources.cmake").string()};
        // 把生效的 C 标准传给 sinterc：它打印的「手动 gcc 链路命令」要跟 CMake 一致，
        // 否则 emit-c 的用户会照着一条没有 -std= 的命令去编译。
        cmd.push_back("--cstd");
        cmd.push_back(cstd.name);
        if (debug) cmd.push_back("--debug");
        cmd.push_back("-I");
        cmd.push_back((cwd / "src").string());
        for (const auto &d: deps) {
            if (d.is_cmake()) continue;
            std::error_code e2;
            if (fs::exists(fs::path(d.path) / "src", e2)) {
                cmd.push_back("-I");
                cmd.push_back((fs::path(d.path) / "src").string());
            }
            cmd.push_back("-I");
            cmd.push_back(d.path);
        }

        std::cout << "generate C sources (no cmake)\n";
        const int rc = run_argv(cmd, true);
        if (rc != 0) {
            std::cerr << "sinterc generate C failed（exit " << rc << "）\n"
                      << "  check SINTERC  line  sinterc：  " << sinterc << "\n";
            return rc == 0 ? 1 : rc;
        }

        // artifactmanifest：call（IDE / ） and ，not  glob。
        const fs::path manifest = gen_dir / "sources.cmake";
        if (!fs::exists(manifest, ec)) {
            std::cerr << "sinterc endno  " << manifest.string() << "\n";
            return 1;
        }
        int files = 0;
        for (const auto &e: fs::directory_iterator(gen_dir, ec))
            if (e.is_regular_file() && e.path().extension() == ".c") ++files;

        std::cout << "\ngenerated:\n"
                  << "  C sources   " << gen_dir.string() << " (" << files << " .c files)\n"
                  << "  manifest    " << manifest.string() << "\n"
                  << "  CMake       " << (dst / "CMakeLists.txt").string() << "\n"
                  << "\n  compile it yourself (or use cmake):\n"
                  << "  cmake -S \"" << dst.string() << "\" -B \"" << (dst / "cmake").string()
                  << "\" -DSINTERC_EXE=\"" << sinterc << "\"\n"
                  << "  cmake --build \"" << (dst / "cmake").string() << "\" -j\n";
        return 0;
    }

    int cmd_info() {
        const fs::path cwd = fs::current_path();
        Manifest m;
        std::string err;
        if (!Manifest::load((cwd / "sinter.toml").string(), m, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        std::cout << m.name << " " << m.version << "\n"
                  << "  kind:     " << package_kind_name(m.kind) << "\n"
                  << "  artifact: " << artifact_name(m.artifact) << "\n"
                  << "  license:  " << (m.license.empty() ? "(unspecified)" : m.license) << "\n";
        if (!m.entry.empty()) std::cout << "  entry:    " << m.entry << "\n";
        if (!m.target.empty()) std::cout << "  target:   " << m.target << "\n";
        std::cout << "  lock:     "
                  << (fs::exists(cwd / "sinter.lock") ? "sinter.lock present" : "not installed")
                  << "\n";
        if (!m.dependencies.empty()) {
            std::cout << "  dependencies:\n";
            for (auto &d: m.dependencies)
                std::cout << "    " << d.name << " " << d.version
                          << (d.source.empty() ? "" : " (" + d.source + ")") << "\n";
        }
        return 0;
    }

    int cmd_clean() {
        const fs::path cwd = fs::current_path();
        std::error_code ec;
        const auto n = fs::remove_all(cwd / "build", ec);
        if (ec) {
            std::cerr << "clean failed: " << ec.message() << "\n";
            return 1;
        }
        std::cout << "removed build/ (" << n << " entr" << (n == 1 ? "y" : "ies") << ")\n";
        return 0;
    }

    int cmd_tree() {
        const fs::path cwd = fs::current_path();
        Manifest m;
        std::string err;
        if (!Manifest::load((cwd / "sinter.toml").string(), m, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        std::vector<ResolvedPackage> deps;
        if (!resolve_or_report(m, cwd, deps, "dependencies")) return 1;
        std::cout << m.name << " " << m.version << "  ["
                  << artifact_name(m.artifact_explicit
                                       ? m.artifact
                                       : (m.is_cmake_package() ? Artifact::StaticLib
                                                               : Artifact::Executable))
                  << "]\n";
        for (const auto &d: deps)
            std::cout << "├── " << d.name << " " << d.version << "  [" << artifact_name(d.artifact)
                      << "]" << (d.target.empty() ? std::string() : "  " + d.target) << "\n";
        if (deps.empty()) std::cout << "└── (dependency)\n";
        return 0;
    }

    int cmd_publish(const std::string &dir) {
        const fs::path target = dir.empty() ? fs::current_path() : fs::path(dir);
        Manifest m;
        std::string err;
        if (!Manifest::load((target / "sinter.toml").string(), m, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        //  ： add  failed ， the 。
        const Artifact art = m.artifact_explicit
                                 ? m.artifact
                                 : (m.is_cmake_package() ? Artifact::StaticLib
                                                         : Artifact::Executable);
        std::error_code ec;
        if (art == Artifact::HeaderOnly) {
            // filepackagerequires include directory 
            const fs::path inc = target / (m.include_dir.empty() ? "include" : m.include_dir);
            if (!fs::exists(inc, ec)) {
                std::cerr << "filepackage checkfailed: include directorynot   " << inc.string() << "\n";
                return 1;
            }
        } else if (m.is_cmake_package()) {
            const fs::path proj = target / (m.cmake_dir.empty() ? "." : m.cmake_dir);
            if (!fs::exists(proj / "CMakeLists.txt", ec)) {
                std::cerr << "CMake package checkfailed: missing " << (proj / "CMakeLists.txt").string() << "\n";
                return 1;
            }
        } else if (art != Artifact::Executable) {
            // Sinter sourcepackage：entry must   file
            const fs::path entry = target / m.entry;
            if (!fs::exists(entry, ec)) {
                std::cerr << "package checkfailed: entry not   " << entry.string() << "\n";
                return 1;
            }
        }
        Registry reg;
        if (!reg.load(err)) {
            std::cerr << err << "\n";
            return 1;
        }
        if (!reg.publish(m, target.string(), err)) {
            std::cerr << err << "\n";
            return 1;
        }
        return 0;
    }

    int cmd_list() {
        ensure_bundled();
        Registry reg;
        std::string err;
        if (!reg.load(err)) {
            std::cerr << err << "\n";
            return 1;
        }
        const auto all = reg.all_latest();
        if (all.empty()) {
            std::cout << "registryis （" << reg.index_path() << "）\n";
            return 0;
        }
        std::cout << "registry " << Registry::home() << "\n\n";
        for (const auto &p: all) {
            const Artifact a = p.artifact;
            std::cout << "  " << p.name << " " << p.version << "  ["
                      << (p.kind == PackageKind::CMake ? "cmake" : "sinter") << ":" << artifact_name(a);
            if (!p.target.empty()) std::cout << " " << p.target;
            std::cout << "]" << (p.description.empty() ? "" : "  — " + p.description) << "\n";
        }
        return 0;
    }

    int cmd_search(const std::string &kw) {
        Registry reg;
        std::string err;
        if (!reg.load(err)) {
            std::cerr << err << "\n";
            return 1;
        }
        int n = 0;
        for (const auto &p: reg.all_latest()) {
            if (p.name.find(kw) != std::string::npos ||
                p.description.find(kw) != std::string::npos) {
                std::cout << "  " << p.name << " " << p.version
                          << (p.description.empty() ? "" : "  — " + p.description) << "\n";
                ++n;
            }
        }
        if (n == 0) std::cout << "no match for \"" << kw << "\"  package\n";
        return 0;
    }
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 0;
    }
    const std::string cmd = argv[1];
    try {
        if (cmd == "init") return cmd_init(argc > 2 ? argv[2] : "");
        if (cmd == "add") {
            if (argc < 3) { std::cerr << "usage: sinter add <package>[@version]\n"; return 1; }
            return cmd_add(argv[2]);
        }
        if (cmd == "remove") {
            if (argc < 3) { std::cerr << "usage: sinter remove <package>\n"; return 1; }
            return cmd_remove(argv[2]);
        }
        if (cmd == "install") return cmd_install();
        if (cmd == "build") {
            bool do_run = false, debug = false;
            int jobs = 0;
            std::string cstd;
            std::string opt_cli;
            std::string toolchain;
            std::vector<std::string> cmake_args;
            for (int i = 2; i < argc; ++i) {
                const std::string a = argv[i];
                if (a == "--run") do_run = true;
                else if (a == "--debug") { debug = true; opt_cli.clear(); }
                else if (a == "--release") { debug = false; opt_cli = "3"; }
                else if (a == "--opt" && i + 1 < argc) opt_cli = argv[++i];
                else if (a.rfind("--opt=", 0) == 0) opt_cli = a.substr(6);
                else if (a == "--cstd" && i + 1 < argc) cstd = argv[++i];
                else if (a.rfind("--cstd=", 0) == 0) cstd = a.substr(7);
                else if (a == "--toolchain" && i + 1 < argc) toolchain = argv[++i];
                else if (a.rfind("--toolchain=", 0) == 0) toolchain = a.substr(12);
                else if (a == "--cmake-arg" && i + 1 < argc) cmake_args.emplace_back(argv[++i]);
                else if (a.rfind("--cmake-arg=", 0) == 0) cmake_args.push_back(a.substr(12));
                else if (a == "-j" && i + 1 < argc) jobs = std::atoi(argv[++i]);
                else if (a.rfind("-j", 0) == 0 && a.size() > 2) jobs = std::atoi(a.c_str() + 2);
                else {
                    std::cerr << "unknown argument: " << a << "\n";
                    return 1;
                }
            }
            return cmd_build(do_run, debug, jobs, cstd, opt_cli, toolchain, cmake_args);
        }
        if (cmd == "emit-c") {
            std::string dir;
            std::string cstd;
            bool debug = false;
            for (int i = 2; i < argc; ++i) {
                const std::string a = argv[i];
                if (a == "--debug") debug = true;
                else if (a == "--release") debug = false;
                else if (a == "--cstd" && i + 1 < argc) cstd = argv[++i];
                else if (a.rfind("--cstd=", 0) == 0) cstd = a.substr(7);
                else if (!a.empty() && a[0] == '-') {
                    std::cerr << "unknown argument: " << a << "\n";
                    return 1;
                } else dir = a;
            }
            return cmd_emit_c(dir, debug, cstd);
        }
        if (cmd == "emit-cmake") {
            std::string dir;
            std::string cstd;
            for (int i = 2; i < argc; ++i) {
                const std::string a = argv[i];
                if (a == "--cstd" && i + 1 < argc) cstd = argv[++i];
                else if (a.rfind("--cstd=", 0) == 0) cstd = a.substr(7);
                else if (!a.empty() && a[0] == '-') {
                    std::cerr << "unknown argument: " << a << "\n";
                    return 1;
                } else dir = a;
            }
            return cmd_emit_cmake(dir, cstd);
        }
        if (cmd == "tree") return cmd_tree();
        if (cmd == "info") return cmd_info();
        if (cmd == "clean") return cmd_clean();
        if (cmd == "publish") {
            std::string dir;
            for (int i = 2; i < argc; ++i) {
                if (std::string(argv[i]) == "--path" && i + 1 < argc) dir = argv[++i];
                else dir = argv[i];
            }
            return cmd_publish(dir);
        }
        if (cmd == "list") return cmd_list();
        if (cmd == "search") {
            if (argc < 3) { std::cerr << "usage: sinter search <keyword>\n"; return 1; }
            return cmd_search(argv[2]);
        }
        if (cmd == "--version" || cmd == "-V") {
            std::cout << "sinter " << kVersion << "\n";
            return 0;
        }
        if (cmd == "--help" || cmd == "-h" || cmd == "help") {
            usage();
            return 0;
        }
        std::cerr << "unknown command: " << cmd << "\n\n";
        usage();
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}