// sinter packagemanifest（sinter.toml）  parse：not dependency
//
// support package：
//
//   1) Sinter package（default）：src/  .sinter source， sinterc compile。
//      [package]
//      name = "mathx"
//      version = "1.2.0"
//      entry = "src/mathx.sinter"
//      artifact = "static"        # artifact，
//
//   2) CMake projectpackage：package a  CMake project（ C/C++ ），
//      no  Sinter source。 kind = "cmake" declaration，and passed target 
//      link  CMake target。
//      [package]
//      name = "fastmath"
//      version = "0.3.1"
//      kind = "cmake"
//      target = "fastmath"
//      # ：package CMakeLists.txt not  directory  path
//      cmake_dir = "."
//
// artifact artifact（ and  kind ，）：
//
//      "exe"     executable。Sinter packagedefault；not   packagelink。
//      "static"  （.a）。CMake packagedefault。
//      "shared"  （.so / .dll / .dylib）。
//      "header"  file，not binary， include directory。
//
// dependency can `target` overrideexport  CMake target：
//      [dependencies]
//      "fastmath" = { version = "^0.3", source = "registry", target = "fastmath_simd" }
//
// ：CMake packagenot  cmake build call，  sinter generate 
//  CMakeLists.txt passed add_subdirectory / find_package ，
// target、dependency、compileoption CMake parse。
#pragma once

#include <string>
#include <vector>

namespace sinter {
    /// packagetype
    enum class PackageKind {
        Sinter,   ///< src/*.sinter， sinterc compile
        CMake,    ///<   CMake project
    };

    /// artifact（generate  CMake targettype，   packagedependency）
    enum class Artifact {
        Executable, ///< executable。dependencya linepackage error
        StaticLib,  ///< 
        SharedLib,  ///< 
        HeaderOnly, ///< file，not binary
    };

    const char *package_kind_name(PackageKind k);
    /// parse kind string；cannot return  false
    bool parse_package_kind(const std::string &s, PackageKind &out);

    const char *artifact_name(Artifact a);
    /// parse artifact string（exe/static/shared/header ）
    bool parse_artifact(const std::string &s, Artifact &out);
    /// the artifactis dependency link
    bool artifact_is_linkable(Artifact a);

    /// 目标 C 标准：`sinter.toml` 的 `[build] cstd`，可被 `--cstd` / `SINTER_CSTD` 覆盖。
    ///
    /// 生成代码是纯 C，但「哪些写法合法」由标准决定：`for (long long i = ...)` 与
    /// 块中间的声明都要求至少 C99，所以 c89/c90 会被明确拒绝，而不是留给 gcc 报一句
    /// 用户看不懂的错。GNU 扩展版（gnu99/gnu11/…）保留 `__attribute__` 一类用法。
    struct CStandard {
        std::string name = "c99";   ///< 默认目标 C99（严格 ISO；可用 gnu99 等覆盖）
        int cmake_version = 99;      ///< `CMAKE_C_STANDARD` 的取值（99/11/17/23）
        bool extensions = false;     ///< `CMAKE_C_EXTENSIONS`（默认严格 C99）

        /// 解析用户写的标准名（大小写不敏感；接受 `c99` / `gnu99` / `99` 三种写法）。
        /// 失败时 error 是完整可读的提示，包含全部允许取值。
        static bool parse(const std::string &text, CStandard &out, std::string &error);
        /// 允许的取值列表（拼进错误提示与 --help）
        static std::string allowed();
        /// 传给编译器的参数（sinterc 打印链路命令时用）
        [[nodiscard]] std::string compiler_flag() const { return "-std=" + name; }
    };

    struct Dependency {
        std::string name;
        std::string version;   // semanticversion or  ">=1.2.0" class
        std::string source;    // registry | path:/xxx | git:/xxx
        /// the dependencyexport  CMake target（ CMake package，overridepackage  target）
        std::string target;
        /// optional CMake condition; empty = always linked
        std::string when;
    };

    /// Extra link inputs applied to the generated target.
    struct LinkSpec {
        std::vector<std::string> libs;      ///< target_link_libraries entries
        std::vector<std::string> subdirs;   ///< add_subdirectory source dirs
        std::vector<std::string> includes;  ///< target_include_directories
        std::vector<std::string> dirs;      ///< target_link_directories
        std::vector<std::string> defs;      ///< target_compile_definitions
        std::vector<std::string> options;   ///< target_link_options
        [[nodiscard]] bool empty() const {
            return libs.empty() && subdirs.empty() && includes.empty() &&
                   dirs.empty() && defs.empty() && options.empty();
        }
    };

    /// One conditional block: emit the spec only when `cond` is true.
    struct LinkCondition {
        std::string cond;   ///< raw CMake expression inside if(...)
        LinkSpec spec;
    };

    struct Manifest {
        std::string name;
        std::string version = "0.1.0";
        std::string entry = "src/main.sinter";
        std::string description;
        std::vector<std::string> authors;
        /// SPDX license identifier; MIT is the project default.
        std::string license = "MIT";
        PackageKind kind = PackageKind::Sinter;
        /// artifact。default kind ：Sinter package -> Exe，CMake package -> StaticLib。
        ///   artifact is 。
        Artifact artifact = Artifact::Executable;
        bool artifact_explicit = false;   ///< manifest    artifact
        /// export  CMake target（//filepackage；linepackage）
        std::string target;
        /// CMake package project path（default "."，i.e. manifest directory）
        std::string cmake_dir = ".";
        /// CMake packagerequires  find_package ， （）
        std::string find_package;
        /// filepackage  include directory（ manifestdirectory），default "include"
        std::string include_dir = "include";
        /// 目标 C 标准（`[build] cstd`）。空 = 默认 c99，由 resolve_c_std 解析。
        std::string c_std;
        /// 默认优化级别（`[build] opt`）。空 = 默认 -O2；可用 --opt / SINTER_OPT 覆盖。
        std::string opt;
        /// 无条件链接/包含设置（`[link]`）
        LinkSpec link;
        /// 平台条件块（`[[link.condition]]` 或 `[link.android]` 等）
        std::vector<LinkCondition> link_conditions;
        std::vector<Dependency> dependencies;

        [[nodiscard]] bool is_cmake_package() const { return kind == PackageKind::CMake; }
        [[nodiscard]] bool is_executable() const { return artifact == Artifact::Executable; }
        [[nodiscard]] bool is_linkable() const { return artifact_is_linkable(artifact); }
        /// not  artifact  ， kind default
        void apply_default_artifact();

        /// columnis  sinter.toml text
        [[nodiscard]] std::string to_toml() const;
        /// parse sinter.toml text；failed return  false and  error
        static bool parse(const std::string &text, Manifest &out, std::string &error);
        /// from fileread
        static bool load(const std::string &path, Manifest &out, std::string &error);
        /// writefile
        bool save(const std::string &path, std::string &error) const;

        /// validate（kind/artifact/target/entry  ）。parse  call，
        ///  load manifest  error。
        bool validate(std::string &error) const;

        /// 解析目标 C 标准：`c_std` 为空则用默认 c99。失败时 error 说明原因
        /// （取值非法、或低于 C99 —— 生成代码必须至少 C99）。
        bool resolve_c_std(CStandard &out, std::string &error) const;
    };

    /// semanticversion：a<b return  -1，etc 0，a>b return  1
    int compare_versions(const std::string &a, const std::string &b);
} // namespace sinter