// package（registry）：line to registry
#pragma once

#include <string>
#include <vector>

namespace sinter {
    struct BundledPackage {
        std::string name;
        std::string version;
        std::string dir;   // registry packages/<name>
    };

    /// return line packagecolumn（compile CMake  SINTER_BUNDLED_DIR）
    std::vector<BundledPackage> bundled_packages();

    ///  not  packageregistry；return （failednot  ）
    int install_bundled(std::string &error);
} // namespace sinter