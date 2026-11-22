// sinter packagemanifest： TOML （[package] / [dependencies] / string and array）
#include "manifest.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sinter {
    namespace {
        std::string trim(const std::string &s) {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
            return s.substr(b, e - b);
        }

        std::string unquote(const std::string &s) {
            if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
                std::string r;
                for (size_t i = 1; i + 1 < s.size(); ++i) {
                    if (s[i] == '\\' && i + 2 < s.size() &&
                        (s[i + 1] == '"' || s[i + 1] == '\\')) {
                        r += s[++i];
                    } else {
                        r += s[i];
                    }
                }
                return r;
            }
            return s;
        }

        std::vector<std::string> parse_array(const std::string &s) {
            std::vector<std::string> out;
            const auto lb = s.find('[');
            const auto rb = s.rfind(']');
            if (lb == std::string::npos || rb == std::string::npos || rb <= lb) return out;
            std::string inner = s.substr(lb + 1, rb - lb - 1);
            std::string cur;
            bool in_str = false;
            for (char c: inner) {
                if (c == '"') {
                    in_str = !in_str;
                    continue;
                }
                if (c == ',' && !in_str) {
                    if (!trim(cur).empty()) out.push_back(trim(cur));
                    cur.clear();
                    continue;
                }
                cur += c;
            }
            if (!trim(cur).empty()) out.push_back(trim(cur));
            return out;
        }

        //  "name" = "1.0.0"  or  name = { version = "1.0", source = "..." }
        bool split_kv(const std::string &line, std::string &key, std::string &value) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) return false;
            key = unquote(trim(line.substr(0, eq)));
            value = trim(line.substr(eq + 1));
            return !key.empty();
        }

        /// parse `{ a = "x", b = "y" }`，return  key -> value（already  ）。
        ///
        ///  `value.find("version")`   field a  ，
        ///   `{ target = "t", version = "1.0" }` will   version parse "t"
        /// （`target`   "target"， `version`  searchwill  `"t"`）。
        ///   `key = "value"`   parse，no 、not 。
        std::vector<std::pair<std::string, std::string>> parse_inline_table(const std::string &s) {
            std::vector<std::pair<std::string, std::string>> out;
            size_t i = s.find('{');
            if (i == std::string::npos) return out;
            ++i;
            const size_t end = s.rfind('}');
            if (end == std::string::npos || end < i) return out;
            while (i < end) {
                // 
                while (i < end && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ','))
                    ++i;
                if (i >= end) break;
                std::string key;
                if (s[i] == '"' || s[i] == '\'') {
                    const char q = s[i++];
                    while (i < end && s[i] != q) key += s[i++];
                    if (i < end) ++i;
                } else {
                    while (i < end && (std::isalnum(static_cast<unsigned char>(s[i])) ||
                                       s[i] == '_' || s[i] == '-'))
                        key += s[i++];
                }
                while (i < end && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                if (i >= end || s[i] != '=') break;
                ++i;
                while (i < end && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                // ：string or 
                std::string val;
                if (i < end && (s[i] == '"' || s[i] == '\'')) {
                    const char q = s[i++];
                    while (i < end && s[i] != q) {
                        if (s[i] == '\\' && i + 1 < end) ++i;
                        val += s[i++];
                    }
                    if (i < end) ++i;
                } else {
                    while (i < end && s[i] != ',' && s[i] != '}') val += s[i++];
                    val = trim(val);
                }
                if (!key.empty()) out.emplace_back(key, val);
            }
            return out;
        }

        std::string get_field(const std::vector<std::pair<std::string, std::string>> &kv,
                              const std::string &key) {
            for (const auto &p: kv)
                if (p.first == key) return p.second;
            return {};
        }

        std::string lower_copy(std::string s) {
            for (char &c: s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        /// Map a short platform section name to a CMake if() expression.
        /// Returns empty for unknown names; callers report a readable error.
        std::string platform_condition(const std::string &name) {
            const std::string n = lower_copy(trim(name));
            if (n == "android") return "ANDROID";
            if (n == "windows" || n == "win32") return "WIN32";
            if (n == "mingw") return "MINGW";
            if (n == "linux")
                return "CMAKE_SYSTEM_NAME STREQUAL \"Linux\" AND NOT ANDROID";
            if (n == "macos" || n == "macosx" || n == "osx" || n == "apple") return "APPLE";
            if (n == "unix") return "UNIX";
            if (n == "msvc") return "MSVC";
            if (n == "gcc") return "CMAKE_C_COMPILER_ID STREQUAL \"GNU\"";
            if (n == "clang") return "CMAKE_C_COMPILER_ID MATCHES \"Clang\"";
            return {};
        }

        std::string toml_escape(const std::string &s) {
            std::string r;
            for (char c: s) {
                if (c == '\\' || c == '"') r += '\\';
                r += c;
            }
            return r;
        }

        void write_string_array(std::ostringstream &o, const char *key,
                                const std::vector<std::string> &xs) {
            if (xs.empty()) return;
            o << key << " = [";
            for (size_t i = 0; i < xs.size(); ++i) {
                if (i) o << ", ";
                o << '"' << toml_escape(xs[i]) << '"';
            }
            o << "]\n";
        }

        int bracket_balance(const std::string &s) {
            int depth = 0;
            bool in_str = false;
            bool esc = false;
            for (char c: s) {
                if (esc) { esc = false; continue; }
                if (in_str && c == '\\') { esc = true; continue; }
                if (c == '"') { in_str = !in_str; continue; }
                if (in_str) continue;
                if (c == '[') ++depth;
                else if (c == ']') --depth;
            }
            return depth;
        }

        bool parse_link_key(LinkSpec &spec, const std::string &key, const std::string &value,
                            int lineno, std::string &error) {
            if (key == "libs") spec.libs = parse_array(value);
            else if (key == "subdirs" || key == "add_subdirectory") spec.subdirs = parse_array(value);
            else if (key == "includes" || key == "include_dirs") spec.includes = parse_array(value);
            else if (key == "dirs" || key == "link_dirs") spec.dirs = parse_array(value);
            else if (key == "defs" || key == "definitions") spec.defs = parse_array(value);
            else if (key == "options" || key == "link_options") spec.options = parse_array(value);
            else {
                error = "line " + std::to_string(lineno) + ": unknown key in [link]: " + key +
                        " (supported: libs, subdirs, includes, dirs, defs, options)";
                return false;
            }
            return true;
        }
    } // namespace

    const char *package_kind_name(const PackageKind k) {
        switch (k) {
            case PackageKind::Sinter: return "sinter";
            case PackageKind::CMake: return "cmake";
        }
        return "sinter";
    }

    bool parse_package_kind(const std::string &s, PackageKind &out) {
        if (s.empty() || s == "sinter") {
            out = PackageKind::Sinter;
            return true;
        }
        if (s == "cmake") {
            out = PackageKind::CMake;
            return true;
        }
        return false;
    }

    const char *artifact_name(const Artifact a) {
        switch (a) {
            case Artifact::Executable: return "exe";
            case Artifact::StaticLib: return "static";
            case Artifact::SharedLib: return "shared";
            case Artifact::HeaderOnly: return "header";
        }
        return "exe";
    }

    bool parse_artifact(const std::string &s, Artifact &out) {
        // ，not to  
        if (s.empty() || s == "exe" || s == "bin" || s == "executable" || s == "app") {
            out = Artifact::Executable;
            return true;
        }
        if (s == "static" || s == "staticlib" || s == "lib") {
            out = Artifact::StaticLib;
            return true;
        }
        if (s == "shared" || s == "cdylib" || s == "dynamic" || s == "dylib" || s == "so") {
            out = Artifact::SharedLib;
            return true;
        }
        if (s == "header" || s == "headeronly" || s == "headers") {
            out = Artifact::HeaderOnly;
            return true;
        }
        return false;
    }

    bool artifact_is_linkable(const Artifact a) {
        return a != Artifact::Executable;
    }

    void Manifest::apply_default_artifact() {
        //  artifact  default：
        //   Sinter package （exe），CMake package  link （static）。
        if (artifact_explicit) return;
        artifact = (kind == PackageKind::CMake) ? Artifact::StaticLib : Artifact::Executable;
    }

    int compare_versions(const std::string &a, const std::string &b) {
        auto split = [](const std::string &v) {
            std::vector<int> parts;
            std::string cur;
            for (char c: v) {
                if (c == '.' || c == '-' || c == '+') {
                    parts.push_back(cur.empty() ? 0 : std::atoi(cur.c_str()));
                    cur.clear();
                } else if (std::isdigit(static_cast<unsigned char>(c))) {
                    cur += c;
                }
            }
            parts.push_back(cur.empty() ? 0 : std::atoi(cur.c_str()));
            return parts;
        };
        auto p = split(a), q = split(b);
        const size_t n = std::max(p.size(), q.size());
        p.resize(n, 0);
        q.resize(n, 0);
        for (size_t i = 0; i < n; ++i) {
            if (p[i] < q[i]) return -1;
            if (p[i] > q[i]) return 1;
        }
        return 0;
    }

    std::string CStandard::allowed() {
        return "c99 / c11 / c17 / c23（严格标准）或 gnu99 / gnu11 / gnu17 / gnu23（GNU 扩展）";
    }

    bool CStandard::parse(const std::string &text, CStandard &out, std::string &error) {
        std::string s;
        for (char ch: text) {
            if (ch == ' ' || ch == '\t') continue;
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        if (s.empty()) {
            out = CStandard{};   // 空 = 用默认值（c99）
            return true;
        }

        bool ext = false;
        std::string digits = s;
        if (digits.rfind("gnu", 0) == 0) {
            ext = true;
            digits = digits.substr(3);
        } else if (digits[0] == 'c') {
            digits = digits.substr(1);
        }

        int version = 0;    // 写进 CMAKE_C_STANDARD 的值
        int year = 0;       // 只用于比较新旧（99/11/17/23 是数字，不能直接比大小）
        if (digits == "99" || digits == "9x") {
            version = 99;
            year = 1999;
        } else if (digits == "11" || digits == "1x") {
            version = 11;
            year = 2011;
        } else if (digits == "17" || digits == "18") {
            version = 17;
            year = 2017;
        } else if (digits == "23" || digits == "2x") {
            version = 23;
            year = 2023;
        } else if (digits == "89" || digits == "90") {
            version = 90;
            year = 1990;
        } else {
            error = "unsupported C standard \"" + text + "\"（可选：" + allowed() + "）";
            return false;
        }

        // 生成代码用了 for 初始化声明与块中间的声明，都是 C99 才合法。
        // 与其让 gcc 报一堆看不懂的错，不如在这里直接说清楚。
        if (year < 1999) {
            error = "C standard \"" + text + "\" is too old: the generated C requires at least C99 "
                    "(for-init declarations and mixed declarations). Use c99 or newer.";
            return false;
        }

        out.name = (ext ? "gnu" : "c") + std::to_string(version);
        out.cmake_version = version;
        out.extensions = ext;
        return true;
    }

    bool Manifest::resolve_c_std(CStandard &out, std::string &error) const {
        return CStandard::parse(c_std, out, error);
    }

    std::string Manifest::to_toml() const {
        std::ostringstream o;
        o << "[package]\n";
        o << "name = \"" << name << "\"\n";
        o << "version = \"" << version << "\"\n";
        // kind  not  default ，manifest
        if (kind == PackageKind::CMake) {
            o << "kind = \"cmake\"\n";
            // CMake packagedefault  static，default
            if (artifact_explicit || artifact != Artifact::StaticLib)
                o << "artifact = \"" << artifact_name(artifact) << "\"\n";
            if (!target.empty()) o << "target = \"" << target << "\"\n";
            if (!cmake_dir.empty() && cmake_dir != ".") o << "cmake_dir = \"" << cmake_dir << "\"\n";
            if (!find_package.empty()) o << "find_package = \"" << find_package << "\"\n";
            if (artifact == Artifact::HeaderOnly && !include_dir.empty() && include_dir != "include")
                o << "include_dir = \"" << include_dir << "\"\n";
        } else {
            if (!entry.empty()) o << "entry = \"" << entry << "\"\n";
            // Sinter packagedefault  exe，default
            if (artifact_explicit || artifact != Artifact::Executable) {
                o << "artifact = \"" << artifact_name(artifact) << "\"\n";
                if (!target.empty()) o << "target = \"" << target << "\"\n";
                if (artifact == Artifact::HeaderOnly && !include_dir.empty() && include_dir != "include")
                    o << "include_dir = \"" << include_dir << "\"\n";
            }
        }
        if (!description.empty()) o << "description = \"" << description << "\"\n";
        if (!license.empty()) o << "license = \"" << license << "\"\n";
        if (!authors.empty()) {
            o << "authors = [";
            for (size_t i = 0; i < authors.size(); ++i) {
                if (i) o << ", ";
                o << "\"" << authors[i] << "\"";
            }
            o << "]\n";
        }
        if (!c_std.empty() || !opt.empty()) {
            o << "\n[build]\n";
            if (!c_std.empty()) o << "cstd = \"" << c_std << "\"\n";
            if (!opt.empty()) o << "opt = \"" << opt << "\"\n";
        }
        if (!dependencies.empty()) {
            o << "\n[dependencies]\n";
            for (const auto &d: dependencies) {
                if (d.source.empty() || d.source == "registry") {
                    if (d.target.empty() && d.when.empty()) {
                        o << "\"" << d.name << "\" = \"" << d.version << "\"\n";
                    } else {
                        o << "\"" << d.name << "\" = { version = \"" << d.version << "\"";
                        if (!d.target.empty()) o << ", target = \"" << d.target << "\"";
                        if (!d.when.empty()) o << ", when = \"" << toml_escape(d.when) << "\"";
                        o << " }\n";
                    }
                } else {
                    o << "\"" << d.name << "\" = { version = \"" << d.version
                      << "\", source = \"" << d.source << "\"";
                    if (!d.target.empty()) o << ", target = \"" << d.target << "\"";
                    if (!d.when.empty()) o << ", when = \"" << toml_escape(d.when) << "\"";
                    o << " }\n";
                }
            }
        }
        if (!link.empty()) {
            o << "\n[link]\n";
            write_string_array(o, "libs", link.libs);
            write_string_array(o, "subdirs", link.subdirs);
            write_string_array(o, "includes", link.includes);
            write_string_array(o, "dirs", link.dirs);
            write_string_array(o, "defs", link.defs);
            write_string_array(o, "options", link.options);
        }
        for (const auto &lc: link_conditions) {
            o << "\n[[link.condition]]\n";
            o << "if = \"" << toml_escape(lc.cond) << "\"\n";
            write_string_array(o, "libs", lc.spec.libs);
            write_string_array(o, "subdirs", lc.spec.subdirs);
            write_string_array(o, "includes", lc.spec.includes);
            write_string_array(o, "dirs", lc.spec.dirs);
            write_string_array(o, "defs", lc.spec.defs);
            write_string_array(o, "options", lc.spec.options);
        }
        return o.str();
    }

    bool Manifest::validate(std::string &error) const {
        if (name.empty()) {
            error = "manifestmissing [package] name";
            return false;
        }

        // ---- 目标 C 标准 ----
        // 在这里校验，保证 sinter.toml 写错时任何命令都在加载阶段就报错，
        // 而不是等 cmake/gcc 抛出一句跟 C 标准无关的话。
        {
            CStandard cs;
            std::string cs_err;
            if (!resolve_c_std(cs, cs_err)) {
                error = cs_err;
                return false;
            }
        }

        // ----   ----
        if (kind == PackageKind::CMake) {
            if (cmake_dir.empty()) {
                error = "CMake package \"" + name + "\"   cmake_dir cannotis ";
                return false;
            }
        } else if (entry.empty()) {
            error = "Sinter package \"" + name + "\" missing entry";
            return false;
        }

        // ---- link conditions ----
        for (const auto &lc: link_conditions) {
            if (trim(lc.cond).empty()) {
                error = "[[link.condition]] missing if/platform";
                return false;
            }
        }
        for (const auto &d: dependencies) {
            if (trim(d.when).empty() && !d.when.empty()) {
                error = "dependency \"" + d.name + "\" has an empty when/if condition";
                return false;
            }
        }

        // ---- artifact ----
        // default kind ； validatedeclaration 。
        const Artifact a = artifact_explicit
                               ? artifact
                               : (kind == PackageKind::CMake ? Artifact::StaticLib : Artifact::Executable);

        if (a == Artifact::Executable) {
            // linepackagenot the exported target：cannot link，will dependency。
            if (!target.empty()) {
                error = "package \"" + name + "\"   artifact = \"exe\"，cannotdeclaration target。" +
                        "  packagelinkplease  artifact = \"static\"  or  \"shared\"";
                return false;
            }
            return true;
        }

        // header packagenot binary，requires target is  CMake  interfacetarget
        if (target.empty()) {
            error = std::string("package \"") + name + "\"   artifact = \"" + artifact_name(a) +
                    "\"，mustdeclaration target（export  CMake target）";
            return false;
        }
        // CMake targetcannot/，then generate  target_link_libraries cannotreference
        for (char c: target) {
            if (std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '\\') {
                error = "package \"" + name + "\"   target invalid: " + target;
                return false;
            }
        }
        if (a == Artifact::HeaderOnly && include_dir.empty()) {
            error = "filepackage \"" + name + "\"   include_dir cannotis ";
            return false;
        }
        return true;
    }

    bool Manifest::parse(const std::string &text, Manifest &out, std::string &error) {
        std::istringstream in(text);
        std::string line;
        std::string section;
        int lineno = 0;
        out = Manifest{};
        while (std::getline(in, line)) {
            ++lineno;
            // comment（   # not comment）
            std::string clean;
            bool in_str = false;
            for (char c: line) {
                if (c == '"') in_str = !in_str;
                if (c == '#' && !in_str) break;
                clean += c;
            }
            clean = trim(clean);
            if (clean.empty()) continue;
            if (clean.size() >= 4 && clean.substr(0, 2) == "[[" && clean.substr(clean.size() - 2) == "]]") {
                const std::string raw = trim(clean.substr(2, clean.size() - 4));
                if (raw == "link.condition") {
                    out.link_conditions.push_back(LinkCondition{});
                    section = "link.condition";
                    continue;
                }
                error = "line " + std::to_string(lineno) + ": unknown array-of-tables [[" + raw +
                        "]] (supported: [[link.condition]])";
                return false;
            }
            if (clean.front() == '[' && clean.back() == ']') {
                const std::string raw = trim(clean.substr(1, clean.size() - 2));
                if (raw == "link") {
                    section = "link";
                    continue;
                }
                if (raw.rfind("link.", 0) == 0) {
                    const std::string platform = raw.substr(5);
                    const std::string cond = platform_condition(platform);
                    if (cond.empty()) {
                        error = "line " + std::to_string(lineno) + ": unknown platform [link." + platform +
                                "] (use android/windows/mingw/linux/macos/unix/msvc/gcc/clang, or "
                                "[[link.condition]] with if = \"...\")";
                        return false;
                    }
                    LinkCondition lc;
                    lc.cond = cond;
                    out.link_conditions.push_back(std::move(lc));
                    section = "link.condition";
                    continue;
                }
                section = raw;
                continue;
            }
            std::string key, value;
            if (!split_kv(clean, key, value)) {
                error = " " + std::to_string(lineno) + " linecannotparse: " + clean;
                return false;
            }
            // Arrays may span lines in normal TOML; keep reading until brackets close.
            if (!value.empty() && value.front() == '[') {
                int depth = bracket_balance(value);
                while (depth > 0 && std::getline(in, line)) {
                    ++lineno;
                    std::string part;
                    bool part_str = false;
                    for (char c: line) {
                        if (c == '"') part_str = !part_str;
                        if (c == '#' && !part_str) break;
                        part += c;
                    }
                    part = trim(part);
                    if (part.empty()) continue;
                    value += " " + part;
                    depth += bracket_balance(part);
                }
                if (depth != 0) {
                    error = "line " + std::to_string(lineno) + ": unterminated array value";
                    return false;
                }
            }
            if (section == "package") {
                if (key == "name") out.name = unquote(value);
                else if (key == "version") out.version = unquote(value);
                else if (key == "entry") out.entry = unquote(value);
                else if (key == "description") out.description = unquote(value);
                else if (key == "license") out.license = unquote(value);
                else if (key == "authors") out.authors = parse_array(value);
                else if (key == "kind") {
                    PackageKind k;
                    if (!parse_package_kind(unquote(value), k)) {
                        error = " " + std::to_string(lineno) + " line kind   sinter  or  cmake";
                        return false;
                    }
                    out.kind = k;
                    if (k == PackageKind::CMake) out.entry.clear();
                } else if (key == "target") out.target = unquote(value);
                else if (key == "cmake_dir") out.cmake_dir = unquote(value);
                else if (key == "find_package") out.find_package = unquote(value);
                else if (key == "include_dir") out.include_dir = unquote(value);
                else if (key == "artifact") {
                    Artifact a;
                    if (!parse_artifact(unquote(value), a)) {
                        error = " " + std::to_string(lineno) +
                                " line artifact   exe / static / shared / header";
                        return false;
                    }
                    out.artifact = a;
                    out.artifact_explicit = true;
                }
            } else if (section == "build") {
                // build 选项：目前只有 cstd（目标 C 标准）。
                // 未知键直接报错 —— 拼错 cstdd 这种静默忽略最难查。
                if (key == "cstd" || key == "c_std") {
                    out.c_std = unquote(value);
                } else if (key == "opt") {
                    out.opt = unquote(value);
                } else {
                    error = "line " + std::to_string(lineno) + ": unknown key in [build]: " + key +
                            " (supported: cstd, opt)";
                    return false;
                }
            } else if (section == "link") {
                if (!parse_link_key(out.link, key, value, lineno, error)) return false;
            } else if (section == "link.condition") {
                if (out.link_conditions.empty()) {
                    error = "line " + std::to_string(lineno) + ": [link.*] data without a header";
                    return false;
                }
                LinkSpec &spec = out.link_conditions.back().spec;
                if (key == "if" || key == "when") {
                    out.link_conditions.back().cond = unquote(value);
                } else if (key == "platform") {
                    const std::string cond = platform_condition(unquote(value));
                    if (cond.empty()) {
                        error = "line " + std::to_string(lineno) + ": unknown platform name: " +
                                unquote(value);
                        return false;
                    }
                    out.link_conditions.back().cond = cond;
                } else if (!parse_link_key(spec, key, value, lineno, error)) {
                    return false;
                }
            } else if (section == "dependencies") {
                Dependency d;
                d.name = key;
                if (value.size() >= 2 && value.front() == '{') {
                    const auto kv = parse_inline_table(value);
                    d.version = get_field(kv, "version");
                    d.source = get_field(kv, "source");
                    d.target = get_field(kv, "target");
                    d.when = get_field(kv, "when");
                    if (d.when.empty()) d.when = get_field(kv, "if");
                    if (d.source.empty()) d.source = "registry";
                    if (d.version.empty()) d.version = "*";
                } else {
                    d.version = unquote(value);
                    d.source = "registry";
                }
                out.dependencies.push_back(std::move(d));
            }
        }
        if (out.name.empty()) {
            error = "manifestmissing [package] name";
            return false;
        }
        // defaultartifact（Sinter -> exe，CMake -> static），validate
        out.apply_default_artifact();
        // validate  function，parse   static，requires out call
        return out.validate(error);
    }

    bool Manifest::load(const std::string &path, Manifest &out, std::string &error) {
        std::ifstream f(path);
        if (!f) {
            error = "cannotopen " + path;
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return parse(ss.str(), out, error);
    }

    bool Manifest::save(const std::string &path, std::string &error) const {
        std::ofstream f(path);
        if (!f) {
            error = "cannot write " + path;
            return false;
        }
        f << to_toml();
        return true;
    }
} // namespace sinter