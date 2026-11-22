// cmake_gen.cpp —— CMakeLists.txt generate
//
// generate project（generate  ）：
//
//   project(<name> LANGUAGES C [CXX])           #  CMake package  CXX
//   add_custom_command(OUTPUT gen/sinter_<name>.c        # 1) Sinter -> C
//                      COMMAND sinterc <entry> -o gen -I ...)
//   add_custom_target(<name>_sinterc ...)                # 2) 
//   add_library(<name>_sinter STATIC <generate  .c column>)    # 3) 
//   add_subdirectory(<cmake package>) / target_link_libraries # 4) link CMake dependency
//   add_executable(<name> < main>)                   # 5)  artifact
//
//     3 ：sinterc callwill   .c（module + all 
// import  module + runtime）， ， CMake  
// ****mustcompilefile。
//
// ： sinterc  artifactmanifest（gen/sources.cmake）， 
//
//   - each build a targetline sinterc（manifest）；
//   - if manifestalready exists， include ，to all .c；
//   - manifestgenerate （），is declaration .c，and 
//     （CMAKE_CONFIGURE_DEPENDS），to column。
//
// 「filenot will  compile」 compile， not 。
#include "cmake_gen.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace sinter {
    namespace {
        /// CMake string： package，  and 
        std::string q(const std::string &s) {
            std::string r = "\"";
            for (char c: s) {
                if (c == '"' || c == '\\') r += '\\';
                r += c;
            }
            r += "\"";
            return r;
        }

        /// Keep ${...} expressions verbatim; resolve other relative paths against
        /// the project root so `add_subdirectory`/`include` work from build/cmake.
        std::string replace_all(std::string s, const std::string &from, const std::string &to) {
            if (from.empty()) return s;
            size_t at = 0;
            while ((at = s.find(from, at)) != std::string::npos) {
                s.replace(at, from.size(), to);
                at += to.size();
            }
            return s;
        }

        std::string link_path(const std::string &root, const std::string &p) {
            if (p.empty()) return p;
            // A user porting a normal CMake snippet expects these to mean the
            // project root, not build/cmake where the generated file lives.
            std::string s = replace_all(p, "${CMAKE_CURRENT_SOURCE_DIR}", root);
            s = replace_all(s, "${CMAKE_SOURCE_DIR}", root);
            if (s.find("${") != std::string::npos || s.find("$ENV{") != std::string::npos)
                return s;
            const fs::path path(s);
            if (path.is_absolute()) return s;
            return (fs::path(root) / path).string();
        }

        /// Relative subdirs keep their layout under the build dir, so
        /// ${CMAKE_CURRENT_BINARY_DIR}/libs/x/include works as in a normal project.
        std::string subdir_binary_dir(const std::string &sub, int &counter) {
            const bool plain = sub.find("${") == std::string::npos &&
                               sub.find("$ENV{") == std::string::npos &&
                               sub.find("..") == std::string::npos &&
                               !fs::path(sub).is_absolute();
            if (!plain) return "${CMAKE_CURRENT_BINARY_DIR}/deps/link_" + std::to_string(counter++);
            std::string rel = sub;
            while (!rel.empty() && (rel.back() == '/' || rel.back() == '\\')) rel.pop_back();
            return "${CMAKE_CURRENT_BINARY_DIR}/" + rel;
        }

        std::string join_quoted(const std::vector<std::string> &xs) {
            std::ostringstream o;
            for (size_t i = 0; i < xs.size(); ++i) {
                if (i) o << " ";
                o << q(xs[i]);
            }
            return o.str();
        }

        std::string join_quoted_paths(const std::string &root,
                                      const std::vector<std::string> &xs) {
            std::ostringstream o;
            for (size_t i = 0; i < xs.size(); ++i) {
                if (i) o << " ";
                o << q(link_path(root, xs[i]));
            }
            return o.str();
        }

        bool has_subdirs(const LinkSpec &sp) { return !sp.subdirs.empty(); }

        /// Emit the commands for one link spec. `counter` keeps add_subdirectory
        /// binary dirs unique across unconditional and conditional blocks.
        void emit_link_spec(std::ostringstream &o, const std::string &root, const LinkSpec &sp,
                            const std::string &lib, const std::string &proj,
                            bool is_exec, bool is_header, bool is_shared, int &counter) {
            for (const auto &sub: sp.subdirs) {
                const std::string src = link_path(root, sub);
                o << "add_subdirectory(" << q(src) << " \""
                  << subdir_binary_dir(sub, counter) << "\")\n";
            }
            const std::string scope = is_header ? "INTERFACE" : "PUBLIC";
            if (!sp.dirs.empty()) {
                o << "target_link_directories(" << lib << " " << scope << " "
                  << join_quoted_paths(root, sp.dirs) << ")\n";
            }
            if (!sp.includes.empty()) {
                o << "target_include_directories(" << lib << " " << scope << " "
                  << join_quoted_paths(root, sp.includes) << ")\n";
            }
            if (!sp.defs.empty()) {
                o << "target_compile_definitions(" << lib << " " << scope << " "
                  << join_quoted(sp.defs) << ")\n";
            }
            if (!sp.libs.empty()) {
                o << "target_link_libraries(" << lib << " " << scope << " "
                  << join_quoted(sp.libs) << ")\n";
            }
            if (!sp.options.empty()) {
                if (is_exec) {
                    o << "target_link_options(" << proj << " PRIVATE " << join_quoted(sp.options)
                      << ")\n";
                } else if (is_header || !is_shared) {
                    o << "target_link_options(" << lib << " INTERFACE " << join_quoted(sp.options)
                      << ")\n";
                } else {
                    o << "target_link_options(" << lib << " PRIVATE " << join_quoted(sp.options)
                      << ")\n";
                }
            }
        }

        /// dependency  Sinter sourcesearchdirectory（sinterc   -I ）
        std::vector<std::string> sinter_include_dirs(const std::vector<ResolvedPackage> &deps) {
            std::vector<std::string> out;
            for (const auto &d: deps) {
                if (d.is_cmake()) continue;   // CMake packagenot  and  sinterc compile
                std::error_code ec;
                if (fs::exists(fs::path(d.path) / "src", ec))
                    out.push_back((fs::path(d.path) / "src").string());
                out.push_back(d.path);
            }
            return out;
        }
    } // namespace

    std::vector<const ResolvedPackage *> cmake_dependencies(const std::vector<ResolvedPackage> &deps) {
        std::vector<const ResolvedPackage *> out;
        for (const auto &d: deps)
            if (d.is_cmake()) out.push_back(&d);
        return out;
    }

    std::string check_cmake_package(const ResolvedPackage &p) {
        std::error_code ec;
        // filepackagenot requires CMakeLists.txt， include directory 
        if (p.artifact == Artifact::HeaderOnly) {
            const fs::path inc = p.include_path.empty() ? fs::path(p.path) / "include"
                                                        : fs::path(p.include_path);
            if (!fs::exists(inc, ec))
                return "filepackage " + p.name + "   include directorynot  : " + inc.string();
            return {};
        }
        const fs::path dir = p.cmake_path.empty() ? fs::path(p.path) : fs::path(p.cmake_path);
        if (!fs::exists(dir, ec))
            return "CMake package " + p.name + "  directorynot  : " + dir.string();
        if (!fs::exists(dir / "CMakeLists.txt", ec))
            return "CMake package " + p.name + " missing CMakeLists.txt（directory " + dir.string() +
                   "， cmake_dir project）";
        if (p.target.empty())
            return "CMake package " + p.name + " no link  target";
        return {};
    }

    CMakeGenResult generate_cmakelists(const std::string &root,
                                       const std::string &bin_dir,
                                       const Manifest &m,
                                       const std::vector<ResolvedPackage> &deps,
                                       const CMakeGenOptions &opt) {
        CMakeGenResult res;
        // root  source（ sinter.toml），bin_dir   CMakeLists.txt  。
        // not  （sinter build  build/cmake），sourcepathmust 
        // root parse，then  entry will  build/cmake/src/main.sinter。
        const std::string &src = root;
        const std::string proj = opt.project_name.empty() ? m.name : opt.project_name;
        const std::string lib = opt.target_name.empty() ? proj + "_sinter" : opt.target_name;
        const Artifact art = opt.artifact;
        const bool is_exe = (art == Artifact::Executable);
        const bool is_header = (art == Artifact::HeaderOnly);

        // artifactfile：line  <proj>；/  ，
        // CMake （ line， build  validate）。
        if (is_exe) {
            res.binary_name = proj;
            res.artifact_path = proj;
        } else if (art == Artifact::StaticLib) {
            res.artifact_path = "lib" + lib + ".a";
        } else if (art == Artifact::SharedLib) {
            res.artifact_path = "lib" + lib + ".so";
        }

        const auto cmake_deps = cmake_dependencies(deps);
        const auto incs = sinter_include_dirs(deps);
        for (const auto &d: cmake_deps) {
            // filepackageno linktarget，include directory 
            if (d->artifact == Artifact::HeaderOnly) continue;
            const std::string bad = check_cmake_package(*d);
            if (!bad.empty()) res.warnings.push_back(bad);
            else res.targets.push_back(d->target);
        }

        std::ostringstream o;
        o << "#  sinter generate —— please （ sinter build / emit-cmake will override）\n";
        o << "#\n";
        o << "#   sinterc: .sinter -> C        （buildline）\n";
        if (!is_header)
            o << "#    :  C -> " << (art == Artifact::SharedLib ? "SHARED" : "STATIC")
              << " " << lib << "\n";
        o << "#    artifact: " << artifact_name(art) << " "
          << (is_header ? std::string("（file，binary）") : res.artifact_path) << "\n";
        o << "#\n";
        o << "# build：\n";
        o << "#   cmake -S " << (src) << " -B " << (bin_dir) << " -DCMAKE_BUILD_TYPE=Release\n";
        o << "#   cmake --build " << (bin_dir) << " -j\n";
        o << "\n";
        o << "cmake_minimum_required(VERSION 3.20)\n\n";

        // CMake dependency / [link] subdirs may be C++; enable CXX then.
        bool link_cxx = has_subdirs(m.link);
        for (const auto &lc: m.link_conditions)
            if (has_subdirs(lc.spec)) link_cxx = true;
        if (!cmake_deps.empty() || link_cxx)
            o << "project(" << proj << " VERSION " << m.version << " LANGUAGES C CXX)\n";
        else
            o << "project(" << proj << " VERSION " << m.version << " LANGUAGES C)\n";
        o << "\n";
        o << "set(CMAKE_C_STANDARD " << opt.c_standard << ")\n";
        o << "set(CMAKE_C_STANDARD_REQUIRED ON)\n";
        o << "set(CMAKE_C_EXTENSIONS " << (opt.c_extensions ? "ON" : "OFF") << ")\n";
        o << "if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n";
        o << "    set(CMAKE_BUILD_TYPE Release CACHE STRING \"buildtype\" FORCE)\n";
        o << "endif()\n\n";

        // ---------------- sinterc ----------------
        o << "# ------------------------------------------------------------- sinterc\n";
        if (!opt.sinterc.empty()) {
            o << "set(SINTERC_EXE " << q(opt.sinterc) << ")\n";
        } else {
            o << "find_program(SINTERC_EXE NAMES sinterc HINTS \"$ENV{SINTERC}\")\n";
        }
        o << "if(NOT SINTERC_EXE)\n";
        o << "    message(FATAL_ERROR\n";
        o << "            \"not to  sinterc：please   PATH， or environment SINTERC，\"\n";
        o << "            \" or   -DSINTERC_EXE=/path/to/sinterc \")\n";
        o << "endif()\n\n";

        // ---------------- path ----------------
        o << "# ---------------------------------------------------------------- path\n";
        o << "set(SINTER_ROOT " << q(src) << ")\n";
        o << "set(SINTER_ENTRY " << q((fs::path(src) / m.entry).string()) << ")\n";
        o << "set(SINTER_GEN_DIR \"${CMAKE_CURRENT_BINARY_DIR}/gen\")\n";
                o << "file(MAKE_DIRECTORY \"${SINTER_GEN_DIR}\")\n\n";

        o << "set(SINTER_INCLUDES\n";
        o << "        " << q((fs::path(src) / "src").string()) << "\n";
        for (const auto &i: incs) o << "        " << q(i) << "\n";
        o << ")\n\n";

        // ---------------- 1) Sinter -> C ----------------
        o << "# ------------------------------------------------ 1) Sinter -> C\n";
        if (is_header) {
            // filepackageno  Sinter source， not requires sinterc
            o << "# filepackage：not  C，export include directoryuse。\n\n";
        } else {
            o << "# sinterc will ：module、all  import  module、runtime，\n";
                        //  GLOB  .sinter is  DEPENDS：filecompile。
            // note： "<dir>" "*.sinter"  args。 "<dir>"/*.sinter，
            // CMake will  "Argument not separated from preceding token" 。
            o << "file(GLOB_RECURSE SINTER_SOURCES CONFIGURE_DEPENDS\n";
            o << "        " << q((fs::path(src) / "src").string()) << " \"*.sinter\")\n";
            for (const auto &d: deps) {
                if (d.is_cmake()) continue;
                const std::string var = "SINTER_DEP_" + d.name;
                o << "file(GLOB_RECURSE " << var << " CONFIGURE_DEPENDS "
                  << q((fs::path(d.path) / "src").string()) << " \"*.sinter\")\n";
                o << "list(APPEND SINTER_SOURCES ${" << var << "})\n";
            }
            o << "\n";
            o << "set(SINTER_ARGS -o \"${SINTER_GEN_DIR}\")\n";
            o << "foreach(inc ${SINTER_INCLUDES})\n";
            o << "    list(APPEND SINTER_ARGS -I \"${inc}\")\n";
            o << "endforeach()\n\n";

            o << "# target：each build sinterc（ CMake   DEPENDS ）。\n";
            o << "add_custom_command(\n";
            o << "        OUTPUT \"${SINTER_GEN_DIR}/sinter_" << m.name << ".c\"\n";
            o << "        COMMAND \"${SINTERC_EXE}\" \"${SINTER_ENTRY}\" ${SINTER_ARGS}\n";
            o << "        DEPENDS \"${SINTER_ENTRY}\" ${SINTER_SOURCES}\n";
            o << "        COMMENT \"sinterc: " << m.entry << " -> C\"\n";
            o << "        VERBATIM)\n";
            o << "add_custom_target(" << proj << "_sinterc DEPENDS \"${SINTER_GEN_DIR}/sinter_"
              << m.name << ".c\")\n\n";
        }

        // ---------------- 2)  （artifacttype） ----------------
        o << "# ------------------------------------------------- 2) target\n";
        if (is_header) {
            // filepackage：INTERFACE ，not compile， include path
            o << "# filepackage：INTERFACE ，  include directoryuse。\n";
            o << "add_library(" << lib << " INTERFACE)\n";
            o << "target_include_directories(" << lib << " INTERFACE\n";
            o << "        " << q((fs::path(src) / (m.include_dir.empty() ? "include" : m.include_dir)).string())
              << "\n";
            for (const auto &pi: opt.public_includes) o << "        " << q(pi) << "\n";
            o << ")\n\n";
        } else {
            o << "# mustcompile .c， sinterc  artifactcolumn。\n";
            o << "# so：each    sinterc manifest。\n";
            o << "#\n";
            o << "# is not  「manifestnot  generate」：will 。a dependency or a \n";
            o << "# module  CMake not will ，manifest  include ，modulenot  \n";
            o << "# filecolumn ，link  undefined reference and not reason。\n";
            // manifestnotexists（e.g.  cmake 、not sinterc）：
            //  CONFIGURE_DEPENDS   glob ，column build 。
            o << "file(GLOB SINTER_GENERATED_SOURCES CONFIGURE_DEPENDS \"${SINTER_GEN_DIR}/sinter_*.c\")\n";
            o << "if(SINTER_SOURCE_LIST)\n";
            o << "    set(SINTER_GENERATED_SOURCES ${SINTER_SOURCE_LIST})\n";
            o << "endif()\n";
            o << "if(NOT SINTER_GENERATED_SOURCES)\n";
            // 占位文件用来保证「至少有一个生成源文件」，但空翻译单元在
            // -pedantic-errors 下是非法的（ISO C 禁止空 TU，gcc 报
            // "ISO C forbids an empty translation unit"）。给它一个 typedef，
            // 既不产生任何符号，也让 -std=c17 -pedantic-errors 干净通过。
            o << "    file(WRITE \"${SINTER_GEN_DIR}/sinter_placeholder.c\" \"/* generated by sinterc at build time */\\ntypedef int sinter_generated_placeholder_t;\\n\")\n";
            o << "    list(APPEND SINTER_GENERATED_SOURCES \"${SINTER_GEN_DIR}/sinter_placeholder.c\")\n";
            o << "endif()\n";

            o << "add_library(" << lib << " "
              << (art == Artifact::SharedLib ? "SHARED" : "STATIC") << " ${SINTER_GENERATED_SOURCES})\n";
            // requirespositionno code。set_property must  add_library  
            // （targetmust ）。
            if (art == Artifact::SharedLib)
                o << "set_property(TARGET " << lib << " PROPERTY POSITION_INDEPENDENT_CODE ON)\n";
            o << "add_dependencies(" << lib << " " << proj << "_sinterc)\n";
            o << "target_include_directories(" << lib << " PUBLIC \"${SINTER_GEN_DIR}\")\n";
            // project src/   include path：`bind C header "native.h"` generate 
            // `#include "native.h"` requires C compile -I to ，file
            //  and  .sinter  （project src/ ）。
            o << "target_include_directories(" << lib << " PUBLIC "
              << q((fs::path(src) / "src").string()) << ")" << char(10);
            for (const auto &pi: opt.public_includes)
                o << "target_include_directories(" << lib << " PUBLIC " << q(pi) << ")\n";
            // POSIX builds need pthread + libm; Windows uses Win32 threads and ws2_32.
            o << "if(WIN32)\n";
            o << "    target_link_libraries(" << lib << " PUBLIC ws2_32)\n";
            o << "else()\n";
            o << "    find_package(Threads REQUIRED)\n";
            o << "    target_link_libraries(" << lib << " PUBLIC m Threads::Threads)\n";
            o << "endif()\n\n";

            if (art == Artifact::SharedLib) {
                //  soname，then dependency executable linenot to 
                o << "# ： SONAME  and outputdirectory，line \n";
                o << "set_target_properties(" << lib << " PROPERTIES\n";
                o << "        OUTPUT_NAME \"" << lib << "\"\n";
                o << "        VERSION \"" << m.version << "\"\n";
                o << "        SOVERSION \"" << m.version << "\")\n\n";
            }
        }

        // ---------------- 3) CMake dependencypackage ----------------
        if (!cmake_deps.empty()) {
            o << "# --------------------------------------------- 3) CMake dependencypackage\n";
            o << "# sourcepackage add_subdirectory to binarydirectory，linkexport target；\n";
            o << "# filepackageno linktarget， filedirectory include。\n";
            o << "# packagedeclaration  find_package， installed  。\n";
            int idx = 0;
            for (const auto &d: cmake_deps) {
                const std::string var = "SINTER_DEP_BUILD_" + std::to_string(idx++);
                std::string when;
                for (const auto &dep: m.dependencies)
                    if (dep.name == d->name) when = dep.when;
                o << "\n# --- " << d->name << " " << d->version << " ("
                  << artifact_name(d->artifact) << ") ---\n";
                if (!when.empty()) o << "if(" << when << ")\n";
                if (d->artifact == Artifact::HeaderOnly) {
                    // filepackage：not  add_subdirectory， include directory
                    if (!d->include_path.empty())
                        o << "target_include_directories(" << lib << " PUBLIC "
                          << q(d->include_path) << ")\n";
                    if (!when.empty()) o << "endif()\n";
                    continue;
                }
                if (!d->find_package.empty())
                    o << "find_package(" << d->find_package << " QUIET)\n";
                o << "if(NOT TARGET " << d->target << ")\n";
                o << "    set(" << var << " \"${CMAKE_CURRENT_BINARY_DIR}/deps/" << d->name << "\")\n";
                o << "    add_subdirectory(" << q(d->cmake_path) << " \"${" << var << "}\")\n";
                o << "endif()\n";
                o << "if(NOT TARGET " << d->target << ")\n";
                o << "    message(FATAL_ERROR\n";
                o << "            \"CMake package " << d->name << " undefinedtarget " << d->target
                  << "（checkits  CMakeLists.txt）\")\n";
                o << "endif()\n";
                if (!when.empty()) o << "endif()\n";
            }
            o << "\n";
        }

        // ---------------- 4)  artifact ----------------
        o << "# ----------------------------------------------- 4)  artifact\n";
        if (is_exe) {
            o << "# module  main   sinter_" << m.name << ".c  ， a \n";
            o << "#  executable and  。\n";
            o << "file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/sinter_entry_stub.c\"\n";
            o << "        \"/* ：  main  sinterc generate  sinter_" << m.name << ".c */\\n\"\n";
            o << "        \"typedef int sinter_entry_stub_t;\\n\")\n";
            o << "add_executable(" << proj << " \"${CMAKE_CURRENT_BINARY_DIR}/sinter_entry_stub.c\")\n";
            o << "target_link_libraries(" << proj << " PRIVATE " << lib << ")\n";
            o << "set_target_properties(" << proj << " PROPERTIES\n";
            o << "        RUNTIME_OUTPUT_DIRECTORY \"${CMAKE_CURRENT_BINARY_DIR}\")\n";
        } else if (art == Artifact::SharedLib) {
            o << "# project： " << res.artifact_path << "\n";
            o << "set_target_properties(" << lib << " PROPERTIES\n";
            o << "        LIBRARY_OUTPUT_DIRECTORY \"${CMAKE_CURRENT_BINARY_DIR}\")\n";
        } else if (art == Artifact::StaticLib) {
            o << "# project： " << res.artifact_path << "\n";
            o << "set_target_properties(" << lib << " PROPERTIES\n";
            o << "        ARCHIVE_OUTPUT_DIRECTORY \"${CMAKE_CURRENT_BINARY_DIR}\")\n";
        } else {
            o << "# fileproject：not binary，export " << lib << " use\n";
        }

        // 体积：每个函数/数据单独成 section，链接时丢弃未被引用的部分。
        // 只加编译选项与链接选项，不改优化级别；MSVC 走默认 /OPT:REF。
        // header-only 包的 lib 是 INTERFACE，不能加 PRIVATE 编译选项。
        if (!is_header) {
            o << "\n# ------------------------------------------------ size: gc sections\n";
            o << "if(CMAKE_C_COMPILER_ID MATCHES \"GNU|Clang\")\n";
            o << "    target_compile_options(" << lib << " PRIVATE -ffunction-sections -fdata-sections)\n";
            if (is_exe) {
                o << "    if(APPLE)\n";
                o << "        target_link_options(" << proj << " PRIVATE -Wl,-dead_strip)\n";
                o << "    elseif(UNIX)\n";
                o << "        target_link_options(" << proj << " PRIVATE -Wl,--gc-sections)\n";
                o << "    endif()\n";
            }
            o << "endif()\n";
        }

        if (!res.targets.empty()) {
            o << "\n# CMake dependencypackage target and  link\n";
            std::vector<std::string> plain;
            std::vector<std::pair<std::string, std::string>> cond_targets;
            for (const auto &t: res.targets) {
                std::string when;
                for (const auto &d: cmake_deps)
                    if (d->target == t)
                        for (const auto &dep: m.dependencies)
                            if (dep.name == d->name) when = dep.when;
                if (when.empty()) plain.push_back(t);
                else cond_targets.emplace_back(t, when);
            }
            if (!plain.empty()) {
                o << "target_link_libraries(" << lib << " PUBLIC\n";
                for (const auto &t: plain) o << "        " << t << "\n";
                o << ")\n";
            }
            for (const auto &ct: cond_targets) {
                o << "if(" << ct.second << ")\n";
                o << "    target_link_libraries(" << lib << " PUBLIC " << ct.first << ")\n";
                o << "endif()\n";
            }
        }

        // ---------------- 5) user link inputs ----------------
        if (!m.link.empty() || !m.link_conditions.empty()) {
            o << "\n# ------------------------------------------- 5) link: libs / includes\n";
            int link_counter = 0;
            if (!m.link.empty())
                emit_link_spec(o, src, m.link, lib, proj, is_exe, is_header,
                               art == Artifact::SharedLib, link_counter);
            for (const auto &lc: m.link_conditions) {
                o << "if(" << lc.cond << ")\n";
                emit_link_spec(o, src, lc.spec, lib, proj, is_exe, is_header,
                               art == Artifact::SharedLib, link_counter);
                o << "endif()\n";
            }
        }

        // executable build line to dependency 
        if (is_exe) {
            o << "\n# build  rpath：dependency  builddirectory \n";
            o << "set_target_properties(" << proj << " PROPERTIES\n";
            o << "        BUILD_RPATH \"${CMAKE_CURRENT_BINARY_DIR}\")\n";
        }

        res.cmakelists = o.str();
        return res;
    }
} // namespace sinter