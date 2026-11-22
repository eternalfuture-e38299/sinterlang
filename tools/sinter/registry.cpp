// sinter packageregistry
#include "registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace sinter {
    namespace {
        std::string env_or(const char *name, const std::string &fallback) {
            const char *v = std::getenv(name);
            return (v && *v) ? std::string(v) : fallback;
        }

        std::string trim(const std::string &s) {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
            return s.substr(b, e - b);
        }

        std::string unquote(const std::string &s) {
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
                return s.substr(1, s.size() - 2);
            return s;
        }
    } // namespace

    std::string Registry::home() {
        const std::string h = env_or("SINTER_HOME", "");
        if (!h.empty()) return h;
        const std::string user = env_or("HOME", ".");
        return (fs::path(user) / ".sinter").string();
    }

    std::string Registry::index_path() const {
        return (fs::path(home()) / "registry" / "index.toml").string();
    }

    bool Registry::load(std::string &error) {
        entries_.clear();
        const fs::path p = index_path();
        if (!fs::exists(p)) return true;   // registry
        std::ifstream f(p);
        if (!f) {
            error = "cannotread " + p.string();
            return false;
        }
        // ：each  packagea  [[package]] 
        std::string line;
        PackageVersion cur;
        auto flush = [&] {
            if (!cur.name.empty()) entries_.push_back(cur);
            cur = {};
        };
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line == "[[package]]") {
                flush();
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = trim(line.substr(0, eq));
            const std::string v = unquote(trim(line.substr(eq + 1)));
            if (k == "name") cur.name = v;
            else if (k == "version") cur.version = v;
            else if (k == "description") cur.description = v;
            else if (k == "path") cur.path = v;
            else if (k == "kind") {
                PackageKind pk;
                if (parse_package_kind(v, pk)) cur.kind = pk;
            } else if (k == "target") cur.target = v;
            else if (k == "cmake_dir") cur.cmake_dir = v.empty() ? "." : v;
            else if (k == "find_package") cur.find_package = v;
            else if (k == "include_dir") cur.include_dir = v.empty() ? "include" : v;
            else if (k == "artifact") {
                Artifact a;
                if (parse_artifact(v, a)) cur.artifact = a;
            }
        }
        flush();
        return true;
    }

    bool Registry::save(std::string &error) const {
        const fs::path p = index_path();
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p);
        if (!f) {
            error = "cannot write " + p.string();
            return false;
        }
        f << "# sinter package（ sinter publish generate）\n";
        for (const auto &e: entries_) {
            f << "\n[[package]]\n";
            f << "name = \"" << e.name << "\"\n";
            f << "version = \"" << e.version << "\"\n";
            if (!e.description.empty()) f << "description = \"" << e.description << "\"\n";
            if (e.kind == PackageKind::CMake) {
                f << "kind = \"cmake\"\n";
                if (!e.target.empty()) f << "target = \"" << e.target << "\"\n";
                if (!e.cmake_dir.empty() && e.cmake_dir != ".")
                    f << "cmake_dir = \"" << e.cmake_dir << "\"\n";
                if (!e.find_package.empty())
                    f << "find_package = \"" << e.find_package << "\"\n";
            }
            // artifact  default （CMake default static，Sinter default exe）
            {
                const Artifact def = (e.kind == PackageKind::CMake) ? Artifact::StaticLib
                                                                  : Artifact::Executable;
                if (e.artifact != def)
                    f << "artifact = \"" << artifact_name(e.artifact) << "\"\n";
            }
            if (e.artifact == Artifact::HeaderOnly && !e.include_dir.empty() &&
                e.include_dir != "include")
                f << "include_dir = \"" << e.include_dir << "\"\n";
            f << "path = \"" << e.path << "\"\n";
        }
        return true;
    }

    std::vector<PackageVersion> Registry::find(const std::string &name) const {
        std::vector<PackageVersion> out;
        for (const auto &e: entries_)
            if (e.name == name) out.push_back(e);
        std::sort(out.begin(), out.end(), [](const PackageVersion &a, const PackageVersion &b) {
            return compare_versions(a.version, b.version) < 0;
        });
        return out;
    }

    std::vector<PackageVersion> Registry::all_latest() const {
        std::map<std::string, PackageVersion> best;
        for (const auto &e: entries_) {
            auto it = best.find(e.name);
            if (it == best.end() || compare_versions(it->second.version, e.version) < 0)
                best[e.name] = e;
        }
        std::vector<PackageVersion> out;
        for (auto &[k, v]: best) out.push_back(v);
        return out;
    }

    bool Registry::publish(const Manifest &m, const std::string &dir, std::string &error) {
        const fs::path src = fs::absolute(dir);
        const fs::path dst = fs::path(home()) / "packages" / m.name / m.version;
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::remove_all(dst, ec);
        fs::copy(src, dst,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "packagefailed: " + ec.message();
            return false;
        }
        //  （version）
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const PackageVersion &e) {
                                          return e.name == m.name && e.version == m.version;
                                      }),
                       entries_.end());
        PackageVersion pv;
        pv.name = m.name;
        pv.version = m.version;
        pv.description = m.description;
        pv.path = dst.string();
        pv.kind = m.kind;
        pv.artifact = m.artifact_explicit
                          ? m.artifact
                          : (m.kind == PackageKind::CMake ? Artifact::StaticLib : Artifact::Executable);
        pv.target = m.target;
        pv.cmake_dir = m.cmake_dir;
        pv.find_package = m.find_package;
        pv.include_dir = m.include_dir;
        entries_.push_back(std::move(pv));
        if (!save(error)) return false;
        std::cout << "已注册 " << m.name << " " << m.version
                  << "（" << (m.is_cmake_package() ? "CMake project" : "Sinter source") << " / "
                  << artifact_name(m.artifact_explicit
                                       ? m.artifact
                                       : (m.is_cmake_package() ? Artifact::StaticLib
                                                               : Artifact::Executable))
                  << "） -> " << dst.string() << "\n";
        return true;
    }

    bool Registry::install(const PackageVersion &pv, std::string &out_dir, std::string &error) {
        const fs::path dst = fs::path(home()) / "packages" / pv.name / pv.version;
        std::error_code ec;
        if (!fs::exists(dst)) {
            fs::create_directories(dst.parent_path(), ec);
            fs::copy(pv.path, dst,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error = "install " + pv.name + " failed: " + ec.message();
                return false;
            }
        }
        out_dir = dst.string();
        return true;
    }

    bool version_satisfies(const std::string &version, const std::string &constraint) {
        std::string c = trim(constraint);
        if (c.empty() || c == "*") return true;
        if (c.rfind(">=", 0) == 0) return compare_versions(version, c.substr(2)) >= 0;
        if (c.rfind("<=", 0) == 0) return compare_versions(version, c.substr(2)) <= 0;
        if (c.rfind(">", 0) == 0) return compare_versions(version, c.substr(1)) > 0;
        if (c.rfind("<", 0) == 0) return compare_versions(version, c.substr(1)) < 0;
        if (c.rfind("^", 0) == 0) {
            // version
            const std::string want = c.substr(1);
            if (compare_versions(version, want) < 0) return false;
            const auto dot = want.find('.');
            const std::string major = dot == std::string::npos ? want : want.substr(0, dot);
            return version.rfind(major + ".", 0) == 0 || version == major;
        }
        if (c.rfind("~", 0) == 0) {
            // version
            const std::string want = c.substr(1);
            if (compare_versions(version, want) < 0) return false;
            const auto d1 = want.find('.');
            const auto d2 = d1 == std::string::npos ? std::string::npos : want.find('.', d1 + 1);
            const std::string prefix = d2 == std::string::npos ? want : want.substr(0, d2);
            return version.rfind(prefix, 0) == 0;
        }
        return compare_versions(version, c) == 0;
    }

    bool resolve_dependencies(const Manifest &root,
                              const std::string &root_dir,
                              Registry &registry,
                              std::vector<ResolvedPackage> &out,
                              std::string &error) {
        out.clear();
        std::set<std::string> seen;      // already parse package（ + ）
        std::set<std::string> visiting;  //  pathparse package

        // from manifest ResolvedPackage。
        // `dep_target`  dependencydeclaration   target（）；is  package  target。
        auto make_resolved = [](const Manifest &dm, const std::string &fallback_name,
                                const fs::path &dir, const std::string &dep_target) {
            ResolvedPackage r;
            r.name = dm.name.empty() ? fallback_name : dm.name;
            r.version = dm.version;
            r.path = dir.string();
            r.kind = dm.kind;
            r.artifact = dm.artifact_explicit
                             ? dm.artifact
                             : (dm.kind == PackageKind::CMake ? Artifact::StaticLib
                                                              : Artifact::Executable);
            // target：classartifact，linepackageis 
            if (artifact_is_linkable(r.artifact)) {
                r.target = dep_target.empty() ? dm.target : dep_target;
            }
            // CMake projectrequires add_subdirectory  path；filepackagenot requires
            if (dm.kind == PackageKind::CMake && r.artifact != Artifact::HeaderOnly) {
                r.cmake_path = (dir / (dm.cmake_dir.empty() ? "." : dm.cmake_dir)).string();
            }
            r.find_package = dm.find_package;
            if (r.artifact == Artifact::HeaderOnly) {
                r.include_path = (dir / (dm.include_dir.empty() ? "include" : dm.include_dir)).string();
            }
            return r;
        };

        // dependency ：linepackagecannot  packagelink
        auto check_dependable = [&](const Manifest &dm, const std::string &dep_name) -> bool {
            const Artifact a = dm.artifact_explicit
                                   ? dm.artifact
                                   : (dm.kind == PackageKind::CMake ? Artifact::StaticLib
                                                                    : Artifact::Executable);
            if (!artifact_is_linkable(a)) {
                error = "dependency " + dep_name + "  artifact = \"" + std::string(artifact_name(a)) +
                        "\"（executable），cannot its packagedependency";
                return false;
            }
            // classartifactmustlinkto a target
            if (dm.kind == PackageKind::CMake && a != Artifact::HeaderOnly && dm.target.empty()) {
                error = "CMake package " + dep_name + " no declaration target，dependency  ";
                return false;
            }
            return true;
        };

        std::function<bool(const Manifest &, const fs::path &)> visit =
                [&](const Manifest &m, const fs::path &dir) -> bool {
            for (const auto &d: m.dependencies) {
                if (seen.count(d.name)) {
                    // dependencynot ，and  not   target override。
                    //  must，then generate  CMake projectwill overridelinktarget。
                    for (auto &r: out) {
                        if (r.name != d.name) continue;
                        if (!d.target.empty() && !r.target.empty() && r.target != d.target) {
                            error = "dependency " + d.name + "    not   target: " +
                                    r.target + "  and  " + d.target;
                            return false;
                        }
                    }
                    continue;
                }
                if (visiting.count(d.name)) {
                    error = "dependency: " + d.name + "   dependency reference";
                    return false;
                }

                // pathdependency：source = "path:../foo"
                if (d.source.rfind("path:", 0) == 0) {
                    const fs::path p = fs::absolute(dir / d.source.substr(5));
                    Manifest dm;
                    std::string err;
                    if (!Manifest::load((p / "sinter.toml").string(), dm, err)) {
                        error = "dependency " + d.name + "  manifestreadfailed: " + err;
                        return false;
                    }
                    // pathdependency validateversion，then  sinter.toml    ^2.0
                    // a  1.0  package will no passed。
                    if (!version_satisfies(dm.version, d.version)) {
                        error = "pathdependency " + d.name + "  version " + dm.version +
                                " not  " + d.version;
                        return false;
                    }
                    if (!check_dependable(dm, d.name)) return false;
                    seen.insert(d.name);
                    visiting.insert(d.name);
                    out.push_back(make_resolved(dm, d.name, p, d.target));
                    if (!visit(dm, p)) return false;
                    visiting.erase(d.name);
                    continue;
                }

                // registrydependency：  version
                auto versions = registry.find(d.name);
                if (versions.empty()) {
                    error = "registry not to dependency " + d.name;
                    return false;
                }
                std::vector<std::string> available;
                const PackageVersion *pick = nullptr;
                for (const auto &v: versions) {
                    available.push_back(v.version);
                    if (version_satisfies(v.version, d.version)) pick = &v;   // ，  a 
                }
                //   「not to  version」，registry to ，
                //  。  versionand column。
                if (!pick) {
                    error = "dependency " + d.name + " no  \"" + d.version + "\"  version（: ";
                    for (size_t i = 0; i < available.size(); ++i) {
                        if (i) error += ", ";
                        error += available[i];
                    }
                    error += "）";
                    return false;
                }
                std::string installed;
                if (!registry.install(*pick, installed, error)) return false;

                const fs::path pkg_dir(installed);
                // registry   kind/target，packagemanifestis （  ）。
                Manifest dm;
                std::string err;
                if (!Manifest::load((pkg_dir / "sinter.toml").string(), dm, err)) {
                    error = "installed package " + d.name + " manifest: " + err;
                    return false;
                }
                if (!check_dependable(dm, d.name)) return false;
                seen.insert(d.name);
                visiting.insert(d.name);
                out.push_back(make_resolved(dm, pick->name, pkg_dir, d.target));
                if (!visit(dm, pkg_dir)) return false;
                visiting.erase(d.name);
            }
            return true;
        };

        if (!visit(root, root_dir)) return false;
        std::sort(out.begin(), out.end(),
                  [](const ResolvedPackage &a, const ResolvedPackage &b) { return a.name < b.name; });
        return true;
    }
} // namespace sinter