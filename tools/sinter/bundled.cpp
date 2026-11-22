// package
#include "bundled.hpp"
#include "manifest.hpp"
#include "registry.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#ifndef SINTER_BUNDLED_DIR
#define SINTER_BUNDLED_DIR "packages"
#endif

namespace fs = std::filesystem;

namespace sinter {
    std::vector<BundledPackage> bundled_packages() {
        std::vector<BundledPackage> out;
        // environmentcompileconstant。
        //
        // compile path buildregistry  path；lineto （its  
        // Android：package installto  /data/app/...、  /data/user/0/<pkg>/files）
        //   pathnot  。Android  and packagepassed SINTER_BUNDLED_DIR
        // line  packages/ directory， i.e. ，no 。
        const char *env = std::getenv("SINTER_BUNDLED_DIR");
        const fs::path root = (env != nullptr && *env != '\0') ? fs::path(env) : fs::path(SINTER_BUNDLED_DIR);
        std::error_code ec;
        if (!fs::exists(root, ec)) return out;
        for (const auto &e: fs::directory_iterator(root, ec)) {
            if (!e.is_directory()) continue;
            const fs::path manifest = e.path() / "sinter.toml";
            if (!fs::exists(manifest)) continue;
            Manifest m;
            std::string err;
            if (!Manifest::load(manifest.string(), m, err)) continue;
            out.push_back({m.name, m.version, e.path().string()});
        }
        return out;
    }

    int install_bundled(std::string &error) {
        const auto bundled = bundled_packages();
        if (bundled.empty()) return 0;
        Registry reg;
        if (!reg.load(error)) return 0;

        // 内置包与安装副本一致吗？按文件逐一看 mtime：升级编译器后
        // 只是**多加了几个 .sinter 文件**（版本号没变）时，旧逻辑会以为
        // 同版本已安装而跳过，于是 `import std.thread` 报 "import not found"。
        auto is_fresh = [](const fs::path &src, const fs::path &dst) -> bool {
            std::error_code ec;
            if (!fs::exists(dst, ec)) return false;
            for (const auto &e: fs::recursive_directory_iterator(src, ec)) {
                if (ec) return false;
                if (!e.is_regular_file()) continue;
                const fs::path rel = fs::relative(e.path(), src, ec);
                if (ec) return false;
                const fs::path target = dst / rel;
                if (!fs::exists(target, ec)) return false;
                if (fs::last_write_time(e.path(), ec) > fs::last_write_time(target, ec)) return false;
                if (ec) return false;
            }
            return true;
        };

        // Bundled packages are seeds, not a subscription: an installed package
        // with the same version is left alone. Bump the package version to
        // release a new std/net independently of the compiler.
        // SINTER_REFRESH_BUNDLED=1 forces the old "copy if newer files" behavior.
        const bool force = std::getenv("SINTER_REFRESH_BUNDLED") != nullptr;
        int added = 0;
        for (const auto &b: bundled) {
            const auto versions = reg.find(b.name);
            const PackageVersion *installed = nullptr;
            for (const auto &v: versions)
                if (v.version == b.version) installed = &v;
            if (installed && (!force || is_fresh(b.dir, installed->path))) continue;
            Manifest m;
            std::string err;
            if (!Manifest::load((fs::path(b.dir) / "sinter.toml").string(), m, err)) continue;
            m.name = b.name;
            m.version = b.version;
            if (!reg.publish(m, b.dir, err)) {
                error = err;
                continue;
            }
            ++added;
        }
        return added;
    }
} // namespace sinter