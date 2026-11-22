// sinter packageregistry： + resolving dependencies + file
#pragma once

#include "manifest.hpp"

#include <map>
#include <string>
#include <vector>

namespace sinter {
    ///   a packageversion
    struct PackageVersion {
        std::string name;
        std::string version;
        std::string description;
        std::string path;      // registry  actualdirectory
        /// packagetype（Sinter sourcepackage / CMake projectpackage）
        PackageKind kind = PackageKind::Sinter;
        /// artifact（exe / static / shared / header）
        Artifact artifact = Artifact::Executable;
        /// export target（package）
        std::string target;
        /// CMake package project path
        std::string cmake_dir = ".";
        std::string find_package;
        /// filepackage  include directory
        std::string include_dir = "include";

        [[nodiscard]] bool is_linkable() const { return artifact_is_linkable(artifact); }
    };

    /// dependency  already parsedependency
    struct ResolvedPackage {
        std::string name;
        std::string version;
        std::string path;      // package  directory（ -I use）
        PackageKind kind = PackageKind::Sinter;
        /// artifact
        Artifact artifact = Artifact::Executable;
        ///    CMake target：dependency  target then overridepackage  target
        std::string target;
        /// CMake package actualprojectdirectory（path / cmake_dir）
        std::string cmake_path;
        /// CMake packagerequires  find_package（）
        std::string find_package;
        /// filepackage  include directory path
        std::string include_path;

        [[nodiscard]] bool is_cmake() const { return kind == PackageKind::CMake; }
        [[nodiscard]] bool is_linkable() const { return artifact_is_linkable(artifact); }
        /// CMake projectrequires add_subdirectory （Sinter packagenot requires）
        [[nodiscard]] bool needs_subdirectory() const {
            return kind == PackageKind::CMake && artifact != Artifact::HeaderOnly;
        }
    };

    class Registry {
    public:
        /// return registrydirectory（$SINTER_HOME  or  ~/.sinter）
        static std::string home();

        /// registryfile：<home>/registry/index.toml
        [[nodiscard]] std::string index_path() const;

        /// read；not   return （not ）
        bool load(std::string &error);

        /// namefindversion，versionreturn 
        [[nodiscard]] std::vector<PackageVersion> find(const std::string &name) const;

        /// allpackage（ each  package version）
        [[nodiscard]] std::vector<PackageVersion> all_latest() const;

        ///  directory（sinter publish --path .）
        bool publish(const Manifest &m, const std::string &dir, std::string &error);

        /// install packageto  <home>/packages/<name>/<version>
        bool install(const PackageVersion &pv, std::string &out_dir, std::string &error);

        [[nodiscard]] const std::vector<PackageVersion> &entries() const { return entries_; }

    private:
        std::vector<PackageVersion> entries_;

        bool save(std::string &error) const;
    };

    /// parsemanifest  dependency（support path:  and  registry），return name result
    bool resolve_dependencies(const Manifest &root,
                             const std::string &root_dir,
                             Registry &registry,
                             std::vector<ResolvedPackage> &out,
                             std::string &error);

    /// version （support "1.2.3"、">=1.0"、"^1.2"、"~1.2.0"、"*"）
    bool version_satisfies(const std::string &version, const std::string &constraint);
} // namespace sinter