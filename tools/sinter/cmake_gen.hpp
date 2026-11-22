// sinter/cmake_gen.hpp —— generate CMake project（sinter build  and  sinter emit-cmake ）
//
// is ：  cmd_build  sinterc + gcc（ std::system），
// emit-cmake 。， 「build 、cmake not  」。
//   CMakeLists generate：
//   - `sinter build` generateprojectto  build/ and i.e. call cmake build；
//   - `sinter emit-cmake` generate，not build。
//  「cannotcompile」 CMake a 。
#pragma once

#include "manifest.hpp"
#include "registry.hpp"

#include <string>
#include <vector>

namespace sinter {
    /// generateoption
    struct CMakeGenOptions {
        /// project（default manifest.name）
        std::string project_name;
        /// sinterc executable（then   CMake   find_program find）
        std::string sinterc;
        /// C compilehint（then  CMake ）
        std::string c_compiler;
        /// 目标 C 标准：`CMAKE_C_STANDARD` / `CMAKE_C_EXTENSIONS`。
        /// 由 `sinter.toml` 的 `[build] cstd` 解析（默认 gnu11），`--cstd` 可覆盖。
        int c_standard = 11;
        bool c_extensions = true;
        /// generate artifact。   build_executable bool：
        /// exe -> add_executable，static -> STATIC ，
        /// shared -> SHARED ，header -> INTERFACE （not binary）。
        Artifact artifact = Artifact::Executable;
        ///  / targetoverride（then  <project>_sinter）
        std::string target_name;
        /// use  include directory（filepackage / C package ）
        std::vector<std::string> public_includes;
    };

    /// generateresult： file，artifact and  package CMake dependency
    struct CMakeGenResult {
        std::string cmakelists;          ///< CMakeLists.txt content
        std::string binary_name;         ///< expected artifactfile（is ，line is name）
        std::string artifact_path;       ///< artifact builddirectory path（libxxx.a / libxxx.so / line）
        std::vector<std::string> targets;///< link  CMake dependencytarget
        std::vector<std::string> warnings;///< hintinfo
    };

    /// generate CMakeLists.txt text。
    /// @param root    projectdirectory（ sinter.toml  directory）  path；
    ///                generate  SINTER_ENTRY / src  parse
    /// @param bin_dir builddirectory（CMakeLists.txt  position）  path；
    ///                 and  root not ，then  root is sourcedirectory out-of-source reference
    /// @param m    projectmanifest
    /// @param deps already parsedependency（resolve_dependencies  result）
    /// @param opt  generateoption
    CMakeGenResult generate_cmakelists(const std::string &root,
                                       const std::string &bin_dir,
                                       const Manifest &m,
                                       const std::vector<ResolvedPackage> &deps,
                                       const CMakeGenOptions &opt);

    /// from already parsedependency  CMake package（ build   and validate）
    std::vector<const ResolvedPackage *> cmake_dependencies(const std::vector<ResolvedPackage> &deps);

    /// check CMake package   （ CMakeLists.txt）。
    /// return ，then  error。
    std::string check_cmake_package(const ResolvedPackage &p);
} // namespace sinter