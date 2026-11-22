// sinter backend: codegen_lifecycle.cpp
#include "backend/codegen.hpp"
#include <cstdio>
#include <algorithm>
#include <iostream>
#include <ranges>

namespace sinter {
    void Codegen::emit_class_ctor_def(const ClassDecl *c) {
        auto it = sem_.classes().find(c->name);
        if (it == sem_.classes().end()) return;
        auto &ci = it->second;

        // 构造形参的 C 名（snake_case，语义层已算好）：
        // 形参表、字段初始化、基类转发、用户构造函数调用全部复用这一份。
        std::vector<std::string> ctor_c_names;
        ctor_c_names.reserve(ci.ctor_params.size());
        for (auto &prm: ci.ctor_params) ctor_c_names.push_back(param_c_name(prm));

        // ---- 反射：`Type(field=value, ...)` ----
        // 每个字段按类型选一个转换：整数 -> from_int、浮点 -> from_num、
        // 布尔 -> from_bool、字符串 -> 原样（借用）、C 字符串 -> from_cstr、
        // 指针 -> 地址、类/结构体 -> 递归调用它自己的 to_str。
        c_funs_ << "sinter_string " << ci.c_name << "_to_str(struct " << ci.c_name
                << "* self) {\n";
        // 显示名：泛型实例要还原成 `Box<i64>` 这种可读形式。
        // 单态化把 `Box<i64>` 改名成 `Box_i64`（`inst->name = req.inst_name`），
        // 直接拿 `ci.name` 拼反射串会把内部名字泄进用户输出（`Box_i64(v=7)`）。
        std::string display_name = ci.name;
        if (generic_table_) {
            std::vector<std::string> targs;
            if (generic_table_->split(ci.name, targs) && !targs.empty()) {
                std::string base = ci.name;
                for (auto &a: targs) {
                    const std::string tail = "_" + a;
                    if (base.size() > tail.size() &&
                        base.compare(base.size() - tail.size(), tail.size(), tail) == 0)
                        base.resize(base.size() - tail.size());
                }
                display_name = base + "<";
                for (size_t i = 0; i < targs.size(); ++i) {
                    if (i) display_name += ", ";
                    display_name += targs[i];
                }
                display_name += ">";
            }
        }
        c_funs_ << "    sinter_string out = sinter_string_from_cstr(\"" << display_name << "(\");\n";
        {
            bool first = true;
            for (auto &fld: ci.fields) {
                if (fld.is_static) continue;
                std::string conv;
                const std::string &t = fld.type_c;
                if (t == "sinter_string") {
                    conv = "self->" + fld.c_name;
                } else if (t == "const char*" || t == "char*") {
                    conv = "sinter_string_from_cstr(self->" + fld.c_name + " ? self->" + fld.c_name +
                           " : \"(null)\")";
                } else if (t == "double" || t == "float") {
                    conv = "sinter_string_from_num((double)self->" + fld.c_name + ")";
                } else if (t == "uint8_t") {
                    conv = "sinter_string_from_bool(self->" + fld.c_name + ")";
                } else if (t.rfind("struct ", 0) == 0 &&
                           t.size() > 8 && t[t.size() - 2] == '*') {
                    // Pointer-to-pointer fields (container storage) have no
                    // single-object text form; keep reflection compilable.
                    conv = "sinter_string_from_cstr(\"<ptr>\")";
                } else if (t.rfind("struct ", 0) == 0) {
                    std::string cn = t.substr(7);
                    while (!cn.empty() && cn.back() == '*') cn.pop_back();
                    bool known = false;
                    for (const auto &kv: sem_.classes()) {
                        if (kv.second.c_name == cn) {
                            known = true;
                            break;
                        }
                    }
                    conv = known ? cn + "_to_str(self->" + fld.c_name + ")"
                                 : "sinter_string_from_cstr(\"<obj>\")";
                } else if (t.find("(*)") != std::string::npos) {
                    // 函数指针没有通用的文本形式，不要强行 cast 成整数。
                    conv = "sinter_string_from_cstr(\"<fn>\")";
                } else {
                    conv = "sinter_string_from_int((int64_t)(self->" + fld.c_name + "))";
                }
                c_funs_ << "    out = sinter_string_concat_take_left(out, sinter_string_from_cstr(\""
                        << (first ? "" : ", ") << fld.name << "=\"));\n";
                c_funs_ << "    out = sinter_string_concat_take_left(out, " << conv << ");\n";
                first = false;
            }
        }
        c_funs_ << "    out = sinter_string_concat_take_left(out, sinter_string_from_cstr(\")\"));\n";
        c_funs_ << "    return out;\n";
        c_funs_ << "}\n\n";

        // 构造
        c_funs_ << "struct " << ci.c_name << "* " << ci.c_name << "_new(";
        if (ci.ctor_params.empty()) c_funs_ << "void";
        for (size_t i = 0; i < ci.ctor_params.size(); ++i) {
            if (i) c_funs_ << ", ";
            c_funs_ << Semantic::param_decl(ci.ctor_params[i].type_c, ctor_c_names[i]);
        }
        c_funs_ << ") {\n";
        c_funs_ << "    struct " << ci.c_name << "* self = (struct " << ci.c_name
                << "*)malloc(sizeof(struct " << ci.c_name << "));\n";
        c_funs_ << "    memset(self, 0, sizeof(*self));\n";
        c_funs_ << "    self->__parent = self;\n";
        c_funs_ << "    self->__type_chain = " << ci.c_name << "_type_chain;\n";
        // vtable 绑定
        if (!ci.vtable_slots.empty())
            c_funs_ << "    self->__vptr = " << ci.c_name << "_vtable;\n";
        // super(...) 调用（同模块或跨模块）
        const ClassInfo *base_ptr = ci.base_class.empty() ? nullptr
                                                          : sem_.find_class(ci.base_class);
        if (base_ptr) {
            const ClassInfo &base = *base_ptr;
            std::string args;
            if (getenv("SINTER_DEBUG_CLASS"))
                std::cerr << "[ctor-gen] " << ci.name << " base_args="
                          << (c->base_args.empty() ? 0 : (int) c->base_args[0].size()) << "\n";
            if (!c->base_args.empty() && !c->base_args[0].empty()) {
                // 有显式 super 参数
                std::vector<std::string> parts;
                for (size_t ai = 0; ai < c->base_args[0].size(); ++ai) {
                    // super 参数常用当前构造参数，直接用其名字
                    const std::string want = ai < base.ctor_params.size()
                                                 ? base.ctor_params[ai].type_c
                                                 : std::string();
                    parts.push_back(upcast_if_needed(c->base_args[0][ai].get(), want));
                }
                for (size_t i = 0; i < parts.size(); ++i) { if (i) args += ", "; args += parts[i]; }
            } else {
                // 默认：将同名构造参数传给基类；没有同名时按位置顺序传递
                std::vector<std::string> parts;
                for (size_t bi = 0; bi < base.ctor_params.size(); ++bi) {
                    const auto &bp = base.ctor_params[bi];
                    bool found = false;
                    for (size_t ci_i = 0; ci_i < ci.ctor_params.size(); ++ci_i)
                        if (ci.ctor_params[ci_i].name == bp.name) {
                            parts.push_back(ctor_c_names[ci_i]);
                            found = true;
                            break;
                        }
                    if (!found && bi < ci.ctor_params.size()) {
                        // 位置回退：子类第 bi 个构造参数
                        parts.push_back(ctor_c_names[bi]);
                        found = true;
                    }
                    if (!found) parts.push_back("0");
                }
                for (size_t i = 0; i < parts.size(); ++i) { if (i) args += ", "; args += parts[i]; }
            }
            // 基类子对象直接内联构造（不能先 new 再按值拷贝：
            // 那样 __base.__vptr 里存的是基类对象地址，会把整套 vtable 布局搞错）
            //
            // 接口是抽象基类（`is_abstract`），没有 `_new` / `_copy` / `_del_inner`，
            // 也永远不能被实例化。它只需要一个合法对象头，所以直接清零即可。
            c_funs_ << "    {\n";
            if (base.is_abstract) {
                c_funs_ << "        memset(&self->__base, 0, sizeof(self->__base));\n";
            } else {
                c_funs_ << "        struct " << base.c_name << "* __b = " << base.c_name
                        << "_new(" << args << ");\n";
                c_funs_ << "        self->__base = *__b;\n";
            }
            // 基类子对象**共享子类的 vtable**。
            //
            // 语义上 `r->__base` 代表的是「一个实际类型为 Rect 的 Shape」，
            // 所以它的虚表必须是 `pv_rect_vtable` —— 这样
            // `desc(r)` 里 `s->__vptr[slot]` 才能分派到 `Rect` 的实现。
            //
            // 安全性依赖 thunk 的形参约定：槽位函数接收 `struct Base* __self`，
            // 需要派生类字段时自己走 `__self->__parent` 找外层对象。
            // （早先这行被删掉、改由调用点传 `__parent` 补位，结果 `Shape*`
            // 形参收到 `Rect*` 越界崩溃 —— 职责放错地方了，现已回正。）
            c_funs_ << "        self->__base.__vptr = " << ci.c_name << "_vtable;\n";
            c_funs_ << "        self->__base.__parent = self;\n";
            c_funs_ << "        self->__base.__type_chain = " << ci.c_name << "_type_chain;\n";
            // 抽象基类走的是 memset 分支，没有 `__b` 这个临时对象
            if (!base.is_abstract) c_funs_ << "        free(__b);\n";
            c_funs_ << "    }\n";
        }
        // 字段初始化（字段名与形参 C 名同为 snake_case，但字段必须带 self-> 前缀）
        for (size_t i = 0; i < ci.ctor_params.size(); ++i)
            c_funs_ << "    self->" << Semantic::to_snake_ident(ci.ctor_params[i].name)
                    << " = " << ctor_c_names[i] << ";\n";
        // 类体里的字段默认值：`var text: str = "x"`。
        // 以前只处理主构造形参，普通字段的初始化器被静默丢弃，构造后字段是空值。
        {
            const auto saved_locals = locals_;
            const auto saved_types = local_types_;
            for (size_t i = 0; i < ci.ctor_params.size(); ++i) {
                locals_[ci.ctor_params[i].name] = ctor_c_names[i];
                local_types_[ci.ctor_params[i].name] = ci.ctor_params[i].type_c;
            }
            for (const auto &m: c->members) {
                auto *v = dynamic_cast<const VarDeclStmt *>(m.get());
                if (!v || v->is_static || !v->init) continue;
                bool from_ctor = false;
                for (auto &p: ci.ctor_params)
                    if (p.name == v->name) { from_ctor = true; break; }
                if (from_ctor) continue;
                c_funs_ << "    self->" << Semantic::to_snake_ident(v->name)
                        << " = " << expr(v->init.get()) << ";\n";
            }
            locals_ = saved_locals;
            local_types_ = saved_types;
        }
        // 构造函数体里 `self.value = x` 这类赋值由用户构造函数显式写出；
        // 若形参名与某个「真实字段」同名（`Box(v: T)` 与字段 `value` 不同名，
        // 但字段名恰好等于形参名时），self-> 前缀已经区分开，不会互相覆盖。
        // 用户构造函数体：`fun Res(i: i64) { ... }` 对应 <类名>_<类名>__<形参类型>。
        // 没有这一步，构造函数里的语句（日志、派生初始化等）会被静默丢弃。
        {
            // 泛型类实例的构造函数登记在**模板名**下（`Array`），不在实例名
            // （`Array_i64`）下 —— 直接用 `c->name` 查会找不到，构造函数体里
            // 用户写的语句就被静默丢弃（实测 `Array` 的 `self.data = buf`
            // 没生成，方法永远读到零值）。
            std::vector<std::string> ctor_owners = {c->name};
            if (generic_table_) {
                std::vector<std::string> targs;
                if (generic_table_->split(c->name, targs) && !targs.empty()) {
                    std::string base = c->name;
                    for (auto &a: targs) {
                        const std::string tail = "_" + a;
                        if (base.size() > tail.size() &&
                            base.compare(base.size() - tail.size(), tail.size(), tail) == 0)
                            base.resize(base.size() - tail.size());
                    }
                    if (!base.empty() && base != c->name) ctor_owners.push_back(base);
                }
            }
            const FunctionInfo *user_ctor = nullptr;
            for (auto &owner_name: ctor_owners) {
                auto cit = sem_.functions().find(owner_name);
                if (cit == sem_.functions().end()) continue;
                // 同名重载（`Array` 有 i64/f64/str 三个实例的构造体）只能靠
                // `c_name` 精确挑选 —— 那正是本实例自己的 `<类>_<类>` 符号。
                // 之前按「形参个数相同」取第一个，`Array_f64` 会拿到
                // `Array_str` 的构造体：实参类型对不上，C 直接报 incompatible。
                const std::string want_c = ci.c_name + "_" + Semantic::to_snake_ident(owner_name);
                for (auto &f: cit->second) {
                    if (f.params.size() != ci.ctor_params.size()) continue;
                    if (f.is_static) continue;
                    if (f.c_name == want_c) { user_ctor = &f; break; }
                }
                if (user_ctor) break;
                // 名字对不上就退回「形参个数相同」的候选（非泛型类的常规路径）。
                for (auto &f: cit->second) {
                    if (f.params.size() != ci.ctor_params.size()) continue;
                    if (f.is_static) continue;
                    user_ctor = &f;
                    break;
                }
                if (user_ctor) break;
            }
            if (user_ctor) {
                std::string call_args;
                for (auto &n: ctor_c_names) {
                    if (!call_args.empty()) call_args += ", ";
                    call_args += n;
                }
                c_funs_ << "    " << user_ctor->c_name << "(self";
                if (!call_args.empty()) c_funs_ << ", " << call_args;
                c_funs_ << ");\n";
            }
        }
        c_funs_ << "    return self;\n}\n\n";

        // 析构
        c_funs_ << "void " << ci.c_name << "_del(struct " << ci.c_name << "* self) {\n";
        c_funs_ << "    if (!self) return;\n";
        // 用户析构
        auto dit = sem_.functions().find("destructor_" + c->name);
        if (dit != sem_.functions().end()) {
            for (auto &f: dit->second)
                if (f.owner_class == c->name)
                    c_funs_ << "    " << f.c_name << "(self);\n";
        }
        // 基类析构：内嵌子对象不能 free 自身（它是宿主对象的一部分）
        // 接口基类是抽象类，没有 `_del_inner`，也不持有任何资源，跳过。
        if (base_ptr && !base_ptr->is_abstract)
            c_funs_ << "    " << base_ptr->c_name << "_del_inner(&self->__base);\n";
        // 无基类时不需要任何占位语句

        c_funs_ << "    free(self);\n}\n\n";

        // 内嵌子对象析构：与 _del 相同，但不 free 自身（由宿主对象负责）
        c_funs_ << "void " << ci.c_name << "_del_inner(struct " << ci.c_name
                << "* self) {\n";
        c_funs_ << "    if (!self) return;\n";
        if (dit != sem_.functions().end()) {
            for (auto &f: dit->second)
                if (f.owner_class == c->name)
                    c_funs_ << "    " << f.c_name << "(self);\n";
        }
        if (base_ptr && !base_ptr->is_abstract)
            c_funs_ << "    " << base_ptr->c_name << "_del_inner(&self->__base);\n";
        c_funs_ << "}\n\n";

        // ------------------------------------------------------------------
        // 复制构造 / 移动构造
        // 生成形态：
        //   struct X* X_copy(struct X* self) { ... }
        //       1) 分配新对象
        //       2) 逐字段默认复制（可被用户 T(const T&) 覆盖）
        //       3) 用户定义了 T(const T&) 则用其 x_copy_body__
        //   struct X* X_move(struct X* self) { ... }
        //       1) 分配新对象
        //       2) 逐字段默认搬运（可被用户 T(T&&) 覆盖）
        //       3) 用户定义了 T(T&&) 则用其 x_move_body__
        //       4) 源对象字段清零（移动语义：被移动后不再持有资源）
        // ------------------------------------------------------------------
        for (int pass = 0; pass < 2; ++pass) {
            const bool is_move = pass == 1;
            const std::string fn = is_move ? "_move" : "_copy";
            const std::string suffix = is_move ? "move" : "copy";
            const bool user_defined = is_move ? (c->move_ctor && c->move_ctor->body)
                                              : (c->copy_ctor && c->copy_ctor->body);

            c_funs_ << "struct " << ci.c_name << "* " << ci.c_name << fn
                    << "(struct " << ci.c_name << "* self) {\n";
            c_funs_ << "    if (!self) return 0;\n";
            c_funs_ << "    struct " << ci.c_name << "* r = (struct " << ci.c_name
                    << "*)malloc(sizeof(*r));\n";
            c_funs_ << "    memset(r, 0, sizeof(*r));\n";
            c_funs_ << "    r->__parent = r;\n";
            c_funs_ << "    r->__type_chain = " << ci.c_name << "_type_chain;\n";
            if (!ci.vtable_slots.empty())
                c_funs_ << "    r->__vptr = " << ci.c_name << "_vtable;\n";
            if (base_ptr && !base_ptr->is_abstract) {
                c_funs_ << "    r->__base = *" << base_ptr->c_name << fn
                        << "(&self->__base);\n";
            }
            if (base_ptr) {
                c_funs_ << "    r->__base.__parent = r;\n";
                c_funs_ << "    r->__base.__type_chain = " << ci.c_name << "_type_chain;\n";
            }
            // 默认逐字段搬/拷（用户自定义体在下面覆盖）
            for (auto &fld: ci.fields) {
                if (fld.is_static) continue;
                c_funs_ << "    r->" << fld.c_name << " = self->" << fld.c_name << ";\n";
            }
            if (user_defined) {
                auto it2 = sem_.functions().find(ci.c_name + fn);
                if (it2 != sem_.functions().end() && !it2->second.empty()) {
                    const std::string body = it2->second.front().c_name + "_body";
                    pending_ctor_impls_.push_back({body, body});
                    c_funs_ << "    " << body << "(self, r);\n";
                }
            }
            if (is_move) {
                // 移动后源对象置空（含基类子对象）
                if (base_ptr)
                    c_funs_ << "    memset(&self->__base, 0, sizeof(self->__base));\n";
                for (auto &fld: ci.fields) {
                    if (fld.is_static) continue;
                    // 字符串字段不能置 0（它是三字段结构体），用空字符串
                    if (fld.type_c == "sinter_string")
                        c_funs_ << "    self->" << fld.c_name
                                << " = sinter_string_from_literal(\"\", 0);\n";
                    else
                        c_funs_ << "    self->" << fld.c_name << " = 0;\n";
                }
            }
            c_funs_ << "    return r;\n}\n\n";
        }
    }

    void Codegen::emit_ctor_impls() {
        // 复制/移动体已在 emit_class_ctor_def 中直接生成调用，这里不再单独产出。
    }

    void Codegen::emit_function_decl_impl(const FunctionDecl *f, bool is_method, const std::string &owner) {
        // 可变参数的原型与定义都要多带一个长度形参
        const bool varargs = f->is_variadic;
        auto it = sem_.functions().find(f->name);
        if (it == sem_.functions().end()) return;

        // 找到匹配这一声明的 FunctionInfo
        const FunctionInfo *fi = nullptr;
        for (auto &cand: it->second) {
            if (cand.decl == f) { fi = &cand; break; }
        }
        if (!fi) return;
        if (fi->is_extern) return;
        // 泛型模板本身不能导出原型：它的形参/返回类型里还写着 `T`，
        // 抄进 C 头文件就是 `error: unknown type name 'T'`。
        // 单态化实例的原型由 emit_function_proto_generic 单独输出，
        // 而泛型类实例成员的类型已被代入（下面 has_tparam 为假），仍然会走到这里。
        if (!f->tparams.empty()) {
            bool unresolved = false;
            for (auto &tp: f->tparams) {
                if (fi->return_type_name.find(tp.name) != std::string::npos) unresolved = true;
                for (auto &prm: fi->params)
                    if (prm.type_c.find(tp.name) != std::string::npos) unresolved = true;
            }
            if (unresolved) return;
        }
        // 泛型实例里的成员：类型参数已代入，直接按 c_name 声明（否则调用点
        // 按 c_name 生成而这里跳过，C 会报隐式声明）。
        const bool generic_instance_member = !fi->template_params.empty() && is_method;

        std::string ret = fi->return_type_name;
        if (ret.empty()) ret = "void";
        for (auto &tp: fi->template_params)
            if (ret == tp) ret = "void";   // 兜底：未单态化的泛型不导出

        if (fi->is_inline) {
            h_funs_ << "static inline ";
        }

        h_funs_ << ret << " " << fi->c_name << "(";
        bool first = true;
        if (is_method && !fi->is_static) {
            h_funs_ << "struct " << class_c_name(owner) << "* this";
            first = false;
        }
        for (auto &prm: fi->params) {
            if (!first) h_funs_ << ", ";
            first = false;
            h_funs_ << Semantic::param_decl(prm.type_c, param_c_name(prm));
        }
        if (varargs) {
            if (!first) h_funs_ << ", ";
            first = false;
            h_funs_ << "int64_t " << varargs_len_name(f->variadic_name);
        }
        if (first) h_funs_ << "void";
        h_funs_ << ");\n";
    }

    void Codegen::emit_function_decl(const FunctionDecl *f, bool is_method, const std::string &owner) {
        const size_t mark = (size_t)h_funs_.tellp();
        emit_function_decl_impl(f, is_method, owner);
        const std::string text = h_funs_.str();
        if (text.size() > mark) {
            h_funs_.str(text.substr(0, mark));
            h_funs_.seekp(0, std::ios::end);
            // inline 函数在头文件里是 static inline，必须对所有 TU 可见。
            // static 方法同理：它没有接收者、也没有 vtable 槽，别的模块
            // 调 `Class.staticFun()` 时只能直接链接符号，不导出就是隐式声明。
            const bool export_proto = f->is_pub || f->is_inline || f->is_static;
            fun_decls_.push_back({export_proto, text.substr(mark)});
        }
    }

    void Codegen::emit_function_def(FunctionDecl *f, bool is_method, const std::string &owner) {
        if (std::getenv("SINTER_TRACE")) std::cerr << "[trace] def " << f->name << "\n";
        auto it = sem_.functions().find(f->name);
        if (it == sem_.functions().end()) return;

        const FunctionInfo *fi = nullptr;
        for (auto &cand: it->second) if (cand.decl == f) { fi = &cand; break; }
        if (!fi) return;

        // 函数别名：生成到目标实现的转发
        if (!fi->aliases_via.empty() && !f->body) {
            std::ostringstream fwd;
            if (fi->is_inline) fwd << "static inline ";
            fwd << (fi->return_type_name.empty() ? "void" : fi->return_type_name) << " "
                << fi->c_name << "(";
            bool ff = true;
            for (auto &pi: fi->params) {
                if (!ff) fwd << ", ";
                ff = false;
                fwd << Semantic::param_decl(pi.type_c, param_c_name(pi));
            }
            if (ff) fwd << "void";
            fwd << ") {\n    ";
            if (fi->return_type_name != "void") fwd << "return ";
            fwd << fi->aliases_via.front() << "(";
            for (size_t i = 0; i < fi->params.size(); ++i) {
                if (i) fwd << ", ";
                fwd << param_c_name(fi->params[i]);
            }
            fwd << ");\n}\n\n";
            c_funs_ << fwd.str();
            return;
        }
        if (!f->body) {
            // 抽象方法（接口/抽象基类声明）也要生成一个占位实现：
            // 它会被写进 vtable，若不生成符号会在链接期报未定义引用。
            if (f->is_abstract) {
                std::ostringstream sig3;
                if (fi->is_inline) sig3 << "static inline ";
                sig3 << (fi->return_type_name.empty() ? "void" : fi->return_type_name) << " "
                     << fi->c_name << "(";
                bool first3 = true;
                if (is_method && !fi->is_static) {
                    sig3 << "struct " << class_c_name(owner) << "* this";
                    first3 = false;
                }
                for (auto &pi: fi->params) {
                    if (!first3) sig3 << ", ";
                    first3 = false;
                    sig3 << Semantic::param_decl(pi.type_c, param_c_name(pi));
                }
                if (first3) sig3 << "void";
                sig3 << ") {\n";
                if (is_method && !fi->is_static) sig3 << "    (void)this;\n";
                for (auto &pi: fi->params) sig3 << "    (void)" << param_c_name(pi) << ";\n";
                if (fi->return_type_name != "void" && !fi->return_type_name.empty()) {
                    if (fi->return_type_name == "double" || fi->return_type_name == "float")
                        sig3 << "    return 0.0;\n";
                    else if (fi->return_type_name == "const char*")
                        sig3 << "    return \"\";\n";
                    else if (fi->return_type_name.back() == '*')
                        sig3 << "    return 0;\n";
                    else
                        sig3 << "    return 0;\n";
                }
                sig3 << "}\n\n";
                c_funs_ << sig3.str();
                return;
            }
            // `= default` 的构造函数/析构函数：生成一个空实现，保证符号存在
            if (f->is_default) {
                std::ostringstream sig2;
                if (fi->is_inline) sig2 << "static inline ";
                sig2 << (fi->return_type_name.empty() ? "void" : fi->return_type_name) << " "
                     << fi->c_name << "(";
                bool first2 = true;
                if (is_method && !fi->is_static) {
                    sig2 << "struct " << class_c_name(owner) << "* this";
                    first2 = false;
                }
                for (auto &pi: fi->params) {
                    if (!first2) sig2 << ", ";
                    first2 = false;
                    sig2 << Semantic::param_decl(pi.type_c, param_c_name(pi));
                }
                if (first2) sig2 << "void";
                sig2 << ") {\n";
                if (is_method && fi->return_type_name == "void") sig2 << "    (void)this;\n";
                sig2 << "}\n\n";
                c_funs_ << sig2.str();
            }
            return;                        // 抽象/接口方法
        }
        if (fi->is_extern) return;         // bind C 只声明
        if (!f->tparams.empty()) return;   // 泛型延后处理


        auto saved_locals = locals_;
        auto saved_types = local_types_;
        auto saved_scopes = scopes_;
        auto saved_class = current_class_;
        auto saved_method = current_method_;
        auto saved_class_c = current_class_c_;
        locals_.clear();
        ref_vars_.clear();
        ref_class_vars_.clear();
        local_types_.clear();
        scopes_.clear();

        current_class_ = is_method ? owner : "";
        current_method_ = f->name;
        current_class_c_ = is_method ? class_c_name(owner) : "";

        // 形参先入 locals_（字段映射时判断同名形参需要它，见下方字段映射段）

        current_return_type_ = fi->return_type_name;
        cur_params_.clear();
        std::ostringstream sig;
        if (fi->is_inline) sig << "static inline ";
        sig << (fi->return_type_name.empty() ? "void" : fi->return_type_name) << " " << fi->c_name << "(";
        bool first = true;
        if (is_method && !fi->is_static) {
            sig << "struct " << current_class_c_ << "* this";
            locals_["this"] = "this";
            local_types_["this"] = "struct " + current_class_c_ + "*";
            first = false;
        }
        // 形参来源：AST 显式形参优先；若 AST 无参数列表而语义层补齐了形参
        // （`fun square : i64 = ...` 这类返回类型重载分支），则按语义层形参生成。
        // C 侧的形参名统一取语义层算好的 snake_case 名（param_c_name）。
        if (!f->params.empty()) {
            for (size_t pi = 0; pi < f->params.size(); ++pi) {
                auto &prm = f->params[pi];
                std::string pt = prm.type ? c_type(prm.type.get()) : "long long";
                // 可变参数：末位形参在 C 侧是指向元素数组的指针
                if (f->is_variadic && !f->variadic_name.empty() && prm.name == f->variadic_name)
                    pt += "*";
                const std::string cn = pi < fi->params.size()
                                           ? param_c_name(fi->params[pi])
                                           : Semantic::to_snake_ident(prm.name);
                if (!first) sig << ", ";
                first = false;
                sig << Semantic::param_decl(pt, cn);
                locals_[prm.name] = cn;
                local_types_[prm.name] = pt;
                if (prm.type && prm.type->is_ref) {
                    ref_vars_.insert(prm.name);
                    if (prm.type->kind == TypeAnnotation::Named && sem_.classes().contains(prm.type->name))
                        ref_class_vars_.insert(prm.name);
                }
                cur_params_.insert(cn);
            }
        } else {
            for (auto &pi: fi->params) {
                const std::string cn = param_c_name(pi);
                if (!first) sig << ", ";
                first = false;
                sig << Semantic::param_decl(pi.type_c, cn);
                locals_[pi.name] = cn;
                local_types_[pi.name] = pi.type_c;
                cur_params_.insert(cn);
            }
        }
        // 可变参数：追加隐式长度形参，函数体内以 `<名>__n` 直接引用
        if (f->is_variadic && !f->variadic_name.empty()) {
            const std::string ln = varargs_len_name(f->variadic_name);
            if (!first) sig << ", ";
            first = false;
            sig << "int64_t " << ln;
            locals_[ln] = ln;
            local_types_[ln] = "int64_t";
            cur_params_.insert(ln);
        }
        if (first) sig << "void";
        sig << ")";

        // 扩展函数 / Kotlin 风格接收者：self（解析器也写作 this）指向第一个形参
        if (!fi->ext_receiver.empty() && !fi->params.empty()) {
            const std::string rn = param_c_name(fi->params[0]);
            locals_["self"] = rn;
            local_types_["self"] = fi->params[0].type_c;
            locals_["this"] = rn;
            local_types_["this"] = fi->params[0].type_c;
        }

        c_funs_ << sig.str() << " {\n";
        indent_ = 1;

        // 方法体内的字段可直接用名字访问
        if (is_method && !fi->is_static) {
            auto ci = sem_.classes().find(owner);
            if (ci != sem_.classes().end()) {
                for (auto &fld: ci->second.fields) {
                    if (fld.name == "this") continue;
                    // 形参同名时优先形参（构造函数体内 this->x = x）
                    // 形参同名时：构造/方法形参是局部变量，字段必须显式写 this->x
                    if (locals_.contains(fld.name)) continue;
                    locals_[fld.name] = "this->" + fld.c_name;
                    local_types_[fld.name] = fld.type_c;
                }
            }
        }

        enter_scope();
        // 注意：字符串形参是「借用」，不登记 RAII —— 由调用方持有并负责释放。
        // 这样 println(s) 不会把调用方的 s 释放掉。
        // 记录函数体作用域深度与 no_raii 标志：return 需要清理到这一层
        fn_scope_depth_ = scopes_.size();
        fn_no_raii_ = f->no_raii;
        stmt(f->body.get());
        leave_scope(f->no_raii);
        fn_scope_depth_ = 0;
        fn_no_raii_ = false;
        indent_ = 0;
        c_funs_ << "}\n\n";

        locals_ = saved_locals;
        local_types_ = saved_types;
        scopes_ = saved_scopes;
        current_class_ = saved_class;
        current_method_ = saved_method;
        current_class_c_ = saved_class_c;
    }

    void Codegen::emit_function_def_generic(FunctionDecl *f,
                                            const std::unordered_map<std::string, std::string> &subst,
                                            bool is_method, const std::string &owner) {
        // 正在生成的泛型函数实例：它的类型实参是 `sinter_gen_*` 一族归约的唯一依据。
        // 模板本体（代码生成阶段已移除）不生成，因此这里只需在出口恢复。
        auto saved_tparams = cur_type_params_;
        auto saved_targs = cur_type_args_;
        if (getenv("SINTER_DEBUG_DISPATCH"))
            std::cerr << "[enter-def] " << f->name << " tparams=" << f->tparams.size() << "\n";
        cur_type_params_ = f->tparams;
        cur_type_args_.clear();
        for (auto &tp: f->tparams) {
            if (tp.is_pack) { cur_type_params_.clear(); cur_type_args_.clear(); break; }
            const auto it = subst.find(tp.name);
            cur_type_args_.push_back(it == subst.end() ? std::string("int64_t") : it->second);
        }

        auto saved_locals = locals_;
        auto saved_types = local_types_;
        auto saved_scopes = scopes_;
        auto saved_class = current_class_;
        auto saved_method = current_method_;
        auto saved_class_c = current_class_c_;
        locals_.clear();
        ref_vars_.clear();
        ref_class_vars_.clear();
        local_types_.clear();
        scopes_.clear();
        cur_params_.clear();
        current_class_ = is_method ? owner : "";
        current_method_ = f->name;
        current_class_c_ = is_method ? class_c_name(owner) : "";

        auto subst_type = [&](const TypeAnnotation *t) -> std::string {
            return subst_type_annotation(t, subst);
        };

        const std::string ret_c = subst_type(f->return_type.get());
        const std::string saved_ret_type = current_return_type_;
        current_return_type_ = ret_c;

        std::vector<std::string> tparams;
        for (auto &tp: f->tparams) tparams.push_back(tp.name);
        std::string fn_base;
        for (char ch: Semantic::to_snake_ident(f->name))
            fn_base += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
        std::string fname;
        if (is_method) {
            // 与原型/调用点一致：用语义层算好的 c_name
            if (const FunctionInfo *dfi = fi_of_decl(f)) fname = dfi->c_name;
        }
        if (fname.empty())
            fname = module_ident_ +
                    (is_method ? "_" + Semantic::to_snake_ident(owner) : std::string()) +
                    "_" + fn_base + generic_pack_suffix(f->tparams, subst);

        std::ostringstream sig;
        sig << ret_c << " " << fname << "(";
        bool first = true;
        if (is_method && !f->is_static) {
            sig << "struct " << current_class_c_ << "* this";
            locals_["this"] = "this";
            local_types_["this"] = "struct " + current_class_c_ + "*";
            first = false;
        }
        // 形参生成：泛型类型参数包（`xs: Args...`）逐参数展开成独立形参
        //   show<i64, str>  ->  show_i64_str(int64_t xs0, sinter_string xs1)
        // 异构包因此不需要打包成单一数组，也不传个数（个数在编译期已知）。
        bool expand_pack = false;
        std::string pack_name, pack_base;
        size_t pack_count = 0;
        for (size_t i = 0; i < f->params.size(); ++i) {
            auto &prm = f->params[i];
            if (f->is_variadic && i + 1 == f->params.size() && prm.type &&
                prm.type->kind == TypeAnnotation::Named) {
                for (auto &tp: f->tparams) {
                    if (tp.is_pack && tp.name == prm.type->name) {
                        expand_pack = true;
                        pack_name = tp.name;
                        pack_base = Semantic::to_snake_ident(prm.name);
                        const auto cnt = subst.find(tp.name + "#count");
                        pack_count = cnt == subst.end() ? 0
                                                        : static_cast<size_t>(std::stoul(cnt->second));
                    }
                }
            }
            if (expand_pack && prm.type->name == pack_name) {
                for (size_t k = 0; k < pack_count; ++k) {
                    const auto a = subst.find(pack_name + "#" + std::to_string(k));
                    const std::string pt = a == subst.end() ? std::string("int64_t") : a->second;
                    const std::string cn = pack_base + std::to_string(k);
                    if (!first) sig << ", ";
                    first = false;
                    sig << Semantic::param_decl(pt, cn);
                    locals_[prm.name + "[" + std::to_string(k) + "]"] = cn;
                    local_types_[prm.name + "[" + std::to_string(k) + "]"] = pt;
                    cur_params_.insert(cn);
                }
                // `xs[i]` 以字面量下标改写，`xs__n` 折叠成常量
                locals_[prm.name] = pack_base;
                local_types_[prm.name] = pack_count ? "long long" : "long long";
                if (pack_count == 0) {
                    if (!first) sig << ", ";
                    first = false;
                    sig << "int64_t " << pack_base << "0";
                    locals_[prm.name + "[0]"] = pack_base + "0";
                    local_types_[prm.name + "[0]"] = "int64_t";
                    cur_params_.insert(pack_base + "0");
                }
                const std::string ln = varargs_len_name(prm.name);
                locals_[ln] = std::to_string(pack_count);
                local_types_[ln] = "int64_t";
                continue;
            }
            std::string pt = subst_type(prm.type.get());
            if (f->is_variadic && !f->variadic_name.empty() && prm.name == f->variadic_name)
                pt += "*";
            const std::string cn = Semantic::to_snake_ident(prm.name);
            if (!first) sig << ", ";
            first = false;
            sig << Semantic::param_decl(pt, cn);
            locals_[prm.name] = cn;
            local_types_[prm.name] = pt;
            if (prm.type && prm.type->is_ref) {
                ref_vars_.insert(prm.name);
                if (prm.type->kind == TypeAnnotation::Named && sem_.classes().contains(prm.type->name))
                    ref_class_vars_.insert(prm.name);
            }
            cur_params_.insert(cn);
        }
        // 非泛型参数包的可变参数：仍用数组 + 个数（保持既有行为）
        if (f->is_variadic && !f->variadic_name.empty() && !expand_pack) {
            const std::string ln = varargs_len_name(f->variadic_name);
            if (!first) sig << ", ";
            first = false;
            sig << "int64_t " << ln;
            locals_[ln] = ln;
            local_types_[ln] = "int64_t";
            cur_params_.insert(ln);
        }
        if (first) sig << "void";
        sig << ")";

        // 扩展函数 / 接收者：self 与 this 都指向首参
        if (!f->ext_receiver.empty() && !f->params.empty()) {
            const std::string rn = Semantic::to_snake_ident(f->params[0].name);
            locals_["self"] = rn;
            locals_["this"] = rn;
            local_types_["self"] = locals_[f->params[0].name];
        }

        c_funs_ << sig.str() << " {\n";
        indent_ = 1;

        // 方法体内字段可裸名访问
        if (is_method && !f->is_static) {
            auto ci = sem_.classes().find(owner);
            if (ci != sem_.classes().end()) {
                for (auto &fld: ci->second.fields) {
                    if (fld.name == "this") continue;
                    if (locals_.contains(fld.name)) continue;
                    locals_[fld.name] = "this->" + fld.c_name;
                    local_types_[fld.name] = fld.type_c;
                }
            }
        }

        enter_scope();
        fn_scope_depth_ = scopes_.size();
        fn_no_raii_ = f->no_raii;
        if (f->body) stmt(f->body.get());
        if (getenv("SINTER_DEBUG_DISPATCH"))
            std::cerr << "[gen] fn=" << f->name << " owner=" << owner
                      << " tparams=" << f->tparams.size()
                      << " targs=" << cur_type_args_.size()
                      << " cur_class=" << current_class_
                      << " subst=" << subst.size() << "\n";
        leave_scope(f->no_raii);
        fn_scope_depth_ = 0;
        fn_no_raii_ = false;
        indent_ = 0;
        c_funs_ << "}\n\n";

        locals_ = saved_locals;
        local_types_ = saved_types;
        scopes_ = saved_scopes;
        current_class_ = saved_class;
        current_method_ = saved_method;
        current_class_c_ = saved_class_c;
        current_return_type_ = saved_ret_type;
        cur_type_params_ = saved_tparams;
        cur_type_args_ = saved_targs;
    }

    void Codegen::emit_function_proto_generic(
        FunctionDecl *f, const std::unordered_map<std::string, std::string> &subst,
        bool is_method, const std::string &owner) {
        std::ostringstream &os = generic_proto_out_ ? *generic_proto_out_ : c_funs_;
        auto subst_type = [&](const TypeAnnotation *t) -> std::string {
            return subst_type_annotation(t, subst);
        };
        std::string fn_base;
        for (char ch: Semantic::to_snake_ident(f->name))
            fn_base += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
        const std::string fname = module_ident_ +
                                  (is_method ? "_" + Semantic::to_snake_ident(owner) : std::string()) +
                                  "_" + fn_base + generic_pack_suffix(f->tparams, subst);

        os << (f->is_inline ? "static inline " : "") << subst_type(f->return_type.get())
                << " " << fname << "(";
        bool first = true;
        bool pack_expanded = false;
        if (is_method && !f->is_static) {
            os << "struct " << class_c_name(owner) << "* this";
            first = false;
        }
        for (size_t i = 0; i < f->params.size(); ++i) {
            auto &prm = f->params[i];
            // 泛型类型参数包：逐参数展开（与定义侧一致）
            bool is_pack = false;
            size_t pack_count = 0;
            std::string pack_base;
            if (f->is_variadic && i + 1 == f->params.size() && prm.type &&
                prm.type->kind == TypeAnnotation::Named) {
                for (auto &tp: f->tparams) {
                    if (tp.is_pack && tp.name == prm.type->name) {
                        is_pack = true;
                        pack_expanded = true;
                        pack_base = Semantic::to_snake_ident(prm.name);
                        const auto cnt = subst.find(tp.name + "#count");
                        pack_count = cnt == subst.end() ? 0
                                                        : static_cast<size_t>(std::stoul(cnt->second));
                    }
                }
            }
            if (is_pack) {
                if (pack_count == 0) {
                    if (!first) os << ", ";
                    first = false;
                    os << "int64_t " << pack_base << "0";
                } else {
                    for (size_t k = 0; k < pack_count; ++k) {
                        if (!first) os << ", ";
                        first = false;
                        const auto a = subst.find(prm.type->name + "#" + std::to_string(k));
                        os << (a == subst.end() ? std::string("int64_t") : a->second)
                                << " " << pack_base << k;
                    }
                }
                continue;
            }
            if (!first) os << ", ";
            first = false;
            std::string pt = subst_type(prm.type.get());
            if (f->is_variadic && !f->variadic_name.empty() && prm.name == f->variadic_name)
                pt += "*";
            os << Semantic::param_decl(pt, Semantic::to_snake_ident(prm.name));
        }
        if (f->is_variadic && !f->variadic_name.empty() && !pack_expanded) {
            if (!first) os << ", ";
            first = false;
            os << "int64_t " << varargs_len_name(f->variadic_name);
        }
        if (first) os << "void";
        os << ");\n";
    }

    void Codegen::collect_generic_instantiations(const Program &p,
                                                 GenericFunctionInstantiations &out) {
        auto try_add = [&](FunctionDecl *g, const std::unordered_map<std::string, std::string> &subst) {
            auto &vec = out[g];
            for (auto &existing: vec) if (existing == subst) return;
            vec.push_back(subst);
        };

        // 收集阶段的局部变量类型表：语义分析已经结束，代码生成还没开始，
        // `local_types_` 是空的。没有这张表，`val p = Point(...); id(p)` 推断
        // 不出 T=Point，会给调用点生成 `id_g_point_ptr` 而定义侧退回 `id_i64`。
        std::vector<std::unordered_map<std::string, std::string> > type_scopes(1);
        auto arg_type = [&](Expression *e) -> std::string {
            if (auto *id = dynamic_cast<IdentifierExpr *>(e)) {
                for (auto it = type_scopes.rbegin(); it != type_scopes.rend(); ++it) {
                    const auto hit = it->find(id->name);
                    if (hit != it->end()) return hit->second;
                }
            }
            return c_type_of_expr(e);
        };

        std::function<void(Expression *)> walk_expr = [&](Expression *e) {
            if (!e) return;
            if (auto *c = dynamic_cast<CallExpr *>(e)) {
                if (auto *id = dynamic_cast<IdentifierExpr *>(c->callee.get())) {
                    auto fit = sem_.functions().find(id->name);
                    if (fit != sem_.functions().end()) {
                        for (auto &fi: fit->second) {
                            auto *decl = fi.decl;
                            if (!decl || decl->tparams.empty()) continue;
                            try_add(decl, generic_subst_for_call(decl, c, arg_type));
                        }
                    }
                } else if (auto *ma = dynamic_cast<MemberAccessExpr *>(c->callee.get())) {
                    // 泛型方法：obj.method<T>(...) / Class.method<T>(...)
                    const std::string recv = c_type_of_expr(ma->object.get());
                    auto fit = sem_.functions().find(ma->member);
                    if (fit != sem_.functions().end()) {
                        for (auto &fi: fit->second) {
                            auto *decl = fi.decl;
                            if (!decl || decl->tparams.empty()) continue;
                            if (fi.is_method) {
                                const ClassInfo *oc = sem_.find_class(fi.owner_class);
                                if (oc && !recv.empty() && recv.back() == '*') {
                                    const std::string want = "struct " + oc->c_name + "*";
                                    if (!is_subclass_of_type(recv, want)) continue;
                                }
                            }
                            try_add(decl, generic_subst_for_call(decl, c, arg_type));
                        }
                    }
                }
                walk_expr(c->callee.get());
                for (auto &a: c->args) walk_expr(a.value.get());
            } else if (auto *b = dynamic_cast<BinaryExpr *>(e)) {
                walk_expr(b->left.get());
                walk_expr(b->right.get());
            } else if (auto *u = dynamic_cast<UnaryExpr *>(e)) {
                walk_expr(u->operand.get());
            } else if (auto *m = dynamic_cast<MemberAccessExpr *>(e)) {
                walk_expr(m->object.get());
            } else if (auto *i = dynamic_cast<IndexAccessExpr *>(e)) {
                walk_expr(i->object.get());
                walk_expr(i->index.get());
            } else if (auto *a = dynamic_cast<AssignmentExpr *>(e)) {
                walk_expr(a->target.get());
                walk_expr(a->value.get());
            } else if (auto *t = dynamic_cast<TernaryExpr *>(e)) {
                walk_expr(t->cond.get());
                walk_expr(t->then_b.get());
                walk_expr(t->else_b.get());
            } else if (auto *expr = dynamic_cast<IfExpr *>(e)) {
                walk_expr(expr->cond.get());
                walk_expr(expr->then_b.get());
                walk_expr(expr->else_b.get());
            } else if (auto *w = dynamic_cast<WhenExpr *>(e)) {
                walk_expr(w->subject.get());
                for (auto &br: w->branches) {
                    for (auto &pt: br.patterns) {
                        walk_expr(pt.value.get());
                        walk_expr(pt.lo.get());
                        walk_expr(pt.hi.get());
                    }
                    walk_expr(br.body.get());
                }
                walk_expr(w->else_branch.get());
            } else if (auto *g = dynamic_cast<GroupingExpr *>(e)) {
                walk_expr(g->expr.get());
            } else if (auto *p = dynamic_cast<PipelineExpr *>(e)) {
                walk_expr(p->left.get());
                walk_expr(p->right.get());
            } else if (auto *st = dynamic_cast<StringTemplateExpr *>(e)) {
                for (auto &pt: st->parts) walk_expr(pt.expr.get());
            }
        };

        std::function<void(Statement *)> walk_stmt = [&](Statement *s) {
            if (!s) return;
            if (auto *b = dynamic_cast<BlockStmt *>(s)) {
                type_scopes.emplace_back();
                for (auto &x: b->statements) walk_stmt(x.get());
                type_scopes.pop_back();
            } else if (auto *v = dynamic_cast<VarDeclStmt *>(s)) {
                std::string t;
                if (v->type) t = sem_.type_to_c(v->type.get());
                else if (v->init) t = arg_type(v->init.get());
                if (!t.empty() && !type_scopes.empty()) type_scopes.back()[v->name] = t;
                if (v->init) walk_expr(v->init.get());
            } else if (auto *r = dynamic_cast<ReturnStmt *>(s)) {
                if (r->value) walk_expr(r->value.get());
            } else if (auto *i = dynamic_cast<IfStmt *>(s)) {
                walk_expr(i->cond.get());
                walk_stmt(i->then_b.get());
                walk_stmt(i->else_b.get());
            } else if (auto *w = dynamic_cast<WhileStmt *>(s)) {
                walk_expr(w->cond.get());
                walk_stmt(w->body.get());
            } else if (auto *fo = dynamic_cast<ForStmt *>(s)) {
                walk_expr(fo->iterable.get());
                walk_stmt(fo->body.get());
            } else if (auto *d = dynamic_cast<DoWhileStmt *>(s)) {
                walk_stmt(d->body.get());
                walk_expr(d->cond.get());
            } else if (auto *e = dynamic_cast<ExprStmt *>(s)) {
                walk_expr(e->expr.get());
            } else if (auto *fn = dynamic_cast<FunctionDecl *>(s)) {
                type_scopes.emplace_back();
                for (auto &prm: fn->params)
                    if (prm.type && !type_scopes.empty())
                        type_scopes.back()[prm.name] = sem_.type_to_c(prm.type.get());
                walk_stmt(fn->body.get());
                type_scopes.pop_back();
            } else if (auto *c = dynamic_cast<ClassDecl *>(s)) {
                for (auto &m: c->members) walk_stmt(m.get());
                if (c->destructor) walk_stmt(c->destructor.get());
            } else if (auto *pb = dynamic_cast<PubBlockStmt *>(s)) {
                for (auto &m: pb->statements) walk_stmt(m.get());
            }
        };

        for (auto &s: p.statements) walk_stmt(s.get());
    }
} // namespace sinter