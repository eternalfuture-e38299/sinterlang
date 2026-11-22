// sinter-lsp: json.hpp ——  JSON type and parse/column
//
// is not  sinterd   protocol.hpp？
//   sinterd  protocol 「line JSON」，；
//   LSP requires /array（e.g.  completion  each   array），
//   so a   JSON ，not dependency。
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sinterlsp {
    /// JSON 。LSP   ，a  tagged union 。
    class Json {
    public:
        enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };

        Json() = default;
        Json(std::nullptr_t) {}                                     // NOLINT
        Json(const bool b) : kind_(Kind::Bool), bool_(b) {}          // NOLINT
        Json(const double d) : kind_(Kind::Number), num_(d) {}       // NOLINT
        Json(const int i) : kind_(Kind::Number), num_(i) {}          // NOLINT
        Json(const char *s) : kind_(Kind::String), str_(s) {}        // NOLINT
        Json(std::string s) : kind_(Kind::String), str_(std::move(s)) {}  // NOLINT

        static Json array() {
            Json j;
            j.kind_ = Kind::Array;
            return j;
        }
        static Json object() {
            Json j;
            j.kind_ = Kind::Object;
            return j;
        }

        [[nodiscard]] Kind kind() const { return kind_; }
        [[nodiscard]] bool is_null() const { return kind_ == Kind::Null; }
        [[nodiscard]] bool is_bool() const { return kind_ == Kind::Bool; }
        [[nodiscard]] bool is_number() const { return kind_ == Kind::Number; }
        [[nodiscard]] bool is_string() const { return kind_ == Kind::String; }
        [[nodiscard]] bool is_array() const { return kind_ == Kind::Array; }
        [[nodiscard]] bool is_object() const { return kind_ == Kind::Object; }

        [[nodiscard]] bool as_bool(const bool dflt = false) const {
            return kind_ == Kind::Bool ? bool_ : dflt;
        }
        [[nodiscard]] double as_number(const double dflt = 0) const {
            return kind_ == Kind::Number ? num_ : dflt;
        }
        [[nodiscard]] int as_int(const int dflt = 0) const {
            return kind_ == Kind::Number ? static_cast<int>(num_) : dflt;
        }
        [[nodiscard]] const std::string &as_string() const { return str_; }
        [[nodiscard]] const std::vector<Json> &as_array() const { return arr_; }
        [[nodiscard]] const std::vector<std::pair<std::string, Json>> &as_object() const {
            return obj_;
        }

        // ----   ----
        Json &set(std::string key, Json v) {
            obj_.emplace_back(std::move(key), std::move(v));
            return *this;
        }
        const Json *find(const std::string &key) const {
            for (const auto &kv: obj_)
                if (kv.first == key) return &kv.second;
            return nullptr;
        }
        /// function：pathnot   or typenot  return 
        std::string get_string(const std::string &key, const std::string &dflt = "") const {
            const Json *v = find(key);
            return v && v->is_string() ? v->as_string() : dflt;
        }
        int get_int(const std::string &key, const int dflt = 0) const {
            const Json *v = find(key);
            return v && v->is_number() ? v->as_int() : dflt;
        }
        bool get_bool(const std::string &key, const bool dflt = false) const {
            const Json *v = find(key);
            return v && v->is_bool() ? v->as_bool() : dflt;
        }
        const Json *get(const std::string &key) const { return find(key); }

        // ---- array ----
        Json &push(Json v) {
            arr_.push_back(std::move(v));
            return *this;
        }
        [[nodiscard]] size_t size() const {
            return kind_ == Kind::Array ? arr_.size() : obj_.size();
        }

        /// columnis  JSON text
        [[nodiscard]] std::string dump() const;

        /// parsefailed return  Kind::Null and  error
        static Json parse(const std::string &text, std::string &error);

    private:
        Kind kind_ = Kind::Null;
        bool bool_ = false;
        double num_ = 0;
        std::string str_;
        std::vector<Json> arr_;
        std::vector<std::pair<std::string, Json>> obj_;
    };

    /// stringcontent（not  ）
    std::string json_escape(const std::string &s);

    ///  numberformattingis  JSON number（integernot ）
    std::string json_number(const double d);
} // namespace sinterlsp