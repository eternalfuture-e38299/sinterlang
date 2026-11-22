// sinter backend: codegen_stmts.cpp
#include "backend/codegen.hpp"
#include <cstdio>
#include <algorithm>
#include <iostream>
#include <ranges>

namespace sinter {
    void Codegen::enter_scope() { scopes_.emplace_back(); scope_cleanup_counts_.push_back(0); }

    void Codegen::emit_scope_exit(size_t idx) {
        if (idx >= scopes_.size()) return;
        auto &vars = scopes_[idx];
        // RAII：逆序销毁持有资源的类实例；析构后置 0，重复清理不会二次释放
        for (auto it = vars.rbegin(); it != vars.rend(); ++it) {
            if (!it->raii) continue;
            // 内建数组 T[N] 是 calloc 出来的堆缓冲，作用域退出时 free。
            // 与字符串一样按值清理：释放后清零，提前退出路径重复清理也安全。
            if (it->kind == "array") {
                // 元素类型带析构：先逆序逐个析构，再释放缓冲。
                // 元素是类引用（`T*`），判空后调用 `<T>_del` 并置 0，
                // 与变量级 RAII 的「清理后置空」保持一致（提前退出也不会二次释放）。
                if (!it->elem_del.empty() && it->elem_count > 0) {
                    emit("for (int64_t __i = " + std::to_string(it->elem_count) +
                         " - 1; __i >= 0; --__i) { if (" + it->c_name + "[__i]) { " +
                         it->elem_del + "(" + it->c_name + "[__i]); " + it->c_name +
                         "[__i] = 0; } }");
                }
                emit("free(" + it->c_name + "); " + it->c_name + " = 0;");
                continue;
            }
            // 字符串是值类型：释放后把 data 清零作为哨兵，
            // sinter_string_free 本身也会检查 owned 标记。
            if (it->kind == "string" || it->type_c == "sinter_string") {
                emit("sinter_string_free(" + it->c_name + "); " + it->c_name +
                     ".data = 0; " + it->c_name + ".len = 0; " + it->c_name +
                     ".cap = 0; " + it->c_name + ".owned = 0;");
                continue;
            }
            emit("if (" + it->c_name + ") { " + it->type_c + "_del(" + it->c_name + "); " +
                 it->c_name + " = 0; }");
        }
        if (idx < scope_cleanup_counts_.size() && scope_cleanup_counts_[idx] > 0) {
            emit("sinter_cleanup_pop(" + std::to_string(scope_cleanup_counts_[idx]) + ");");
            scope_cleanup_counts_[idx] = 0;
        }
    }

    void Codegen::emit_scope_exits(size_t from_depth, bool no_raii) {
        if (no_raii) return;
        // 从最内层逆序逐层清理；from_depth 那一层也算在内（它即将被跳过）
        for (size_t i = scopes_.size(); i > from_depth; --i) {
            emit_scope_exit(i - 1);
            // 这一层已被清理，块结束时不再重复生成析构。
            exited_scopes_.insert(i - 1);
        }
    }

    void Codegen::leave_scope(bool no_raii) {
        if (scopes_.empty()) return;
        const size_t idx = scopes_.size() - 1;
        // 若这一层已在 return/break/continue 的提前退出路径中被清理过
        // （块内所有出口都是提前退出），则不再重复输出析构。
        if (!no_raii && !exited_scopes_.contains(idx)) emit_scope_exit(idx);
        exited_scopes_.erase(idx);
        scopes_.pop_back();
        if (!scope_cleanup_counts_.empty()) scope_cleanup_counts_.pop_back();
    }

    const ClassInfo *Codegen::class_by_c_type(const std::string &c_type) const {
        if (c_type.empty() || c_type.back() != '*') return nullptr;
        for (auto &[cn, ci]: sem_.classes())
            if ("struct " + ci.c_name + "*" == c_type) return &ci;
        for (auto &[cn, ci]: sem_.external_classes())
            if ("struct " + ci.c_name + "*" == c_type) return &ci;
        return nullptr;
    }

    void Codegen::register_raii(const std::string &name, const std::string &c_name,
                                const std::string &c_type) {
        if (scopes_.empty()) return;
        // 内部字符串是值类型资源：无条件登记，作用域退出时释放堆缓冲。
        // 字面量（owned=0）在 free 时自动跳过，因此登记成本为零。
        if (c_type == "sinter_string") {
            scopes_.back().push_back({name, c_name, "sinter_string", true, "string"});
            emit("sinter_cleanup_push(&" + c_name + ", sinter_cleanup_string);");
            scope_cleanup_counts_.back()++;
            return;
        }
        const ClassInfo *ci = class_by_c_type(c_type);
        // 只在类声明了析构器时登记。
        // 没有析构器的类只持有 malloc 的内存，看似可以无条件 free，
        // 但这类对象常被存入静态字段或被他处别名引用（nullsafe / static_field
        // 等场景），提前释放会导致 use-after-free。内存与生命周期语义的
        // 完整答案需要所有权分析，不在本轮范围内，因此保持保守策略。
        if (!ci || !ci->has_destructor) return;
        scopes_.back().push_back({name, c_name, ci->c_name, true, "class"});
        emit("sinter_cleanup_push(&" + c_name + ", " + ci->c_name + "_cleanup);");
        scope_cleanup_counts_.back()++;
    }

    void Codegen::stmt(Statement *s) {
        if (!s) return;
        if (auto *b = dynamic_cast<BlockStmt *>(s)) { stmt_block(b); return; }
        if (auto *v = dynamic_cast<VarDeclStmt *>(s)) { stmt_var(v); return; }
        if (auto *d = dynamic_cast<DestructureStmt *>(s)) { stmt_destructure(d); return; }
        if (auto *r = dynamic_cast<ReturnStmt *>(s)) { stmt_return(r); return; }
        if (auto *th = dynamic_cast<ThrowStmt *>(s)) { stmt_throw(th); return; }
        if (auto *tr = dynamic_cast<TryStmt *>(s)) { stmt_try(tr); return; }
        if (auto *i = dynamic_cast<IfStmt *>(s)) { stmt_if(i); return; }
        if (auto *w = dynamic_cast<WhileStmt *>(s)) { stmt_while(w); return; }
        if (auto *f = dynamic_cast<ForStmt *>(s)) { stmt_for(f); return; }
        if (auto *d = dynamic_cast<DoWhileStmt *>(s)) { stmt_do_while(d); return; }
        if (auto *d = dynamic_cast<DelStmt *>(s)) { stmt_del(d); return; }
        // break / continue 跳过内层块的 leave_scope：只清理循环体内的层，
        // 循环自身那层（loop_scope_depths_.back()）的变量在循环结束后统一析构。
        if (dynamic_cast<BreakStmt *>(s) || dynamic_cast<ContinueStmt *>(s)) {
            // break/continue 会离开循环体，因此循环体那层及其内层都必须清理。
            // from_depth 是循环体作用域的下标（含）。
            if (!loop_scope_depths_.empty()) {
                emit_active_finally(false, loop_scope_depths_.size());
                emit_scope_exits(loop_scope_depths_.back(), false);
            }
            const bool is_cont = dynamic_cast<ContinueStmt *>(s) != nullptr;
            // do-while 的条件被挪到循环体末尾时，continue 必须 goto 到那段代码，
            // 否则会跳过本轮的条件求值（C 的 continue 只会跳到条件处）。
            if (is_cont && !loop_continue_labels_.empty() && !loop_continue_labels_.back().empty())
                emit("goto " + loop_continue_labels_.back() + ";");
            else
                emit(is_cont ? "continue;" : "break;");
            return;
        }
        if (auto *e = dynamic_cast<ExprStmt *>(s)) {
            // 语句里求值出来的字符串临时值需要有人释放。两条来源：
            // ① 整条语句本身求值为字符串（`foo()` 返回 str 却没人接）；
            // ② 调用实参里的临时字符串（`println("a" + b)`）。
            // 两者都先落到局部变量，语句执行完立即释放，等价于给这段 C
            // 补上一个作用域。① 的结果也要进 owned：赋值语句的类型是标量
            // （c_type_of_expr 的兜底），走到这里的字符串结果都是被丢弃的临时值。
            std::vector<std::string> owned = hoist_temp_str_args(e->expr.get());
            // 分派族里的「追加文本」助手返回 void（`sinter_gen_text_sb_item`），
            // 类型推断在这里不可靠，直接按语句处理，别包成临时字符串。
            bool void_stmt = false;
            if (auto *c0 = dynamic_cast<CallExpr *>(e->expr.get()))
                if (auto *id0 = dynamic_cast<IdentifierExpr *>(c0->callee.get()))
                    if (id0->name.find("_sb_item") != std::string::npos) void_stmt = true;
            if (!void_stmt && c_type_of_expr(e->expr.get()) == "sinter_string") {
                const std::string t = fresh("__tmpstr");
                emit("sinter_string " + t + " = " + expr(e->expr.get()) + ";");
                owned.push_back(t);
            } else {
                const std::string x = expr(e->expr.get());
                if (!x.empty()) emit(x + ";");
            }
            finish_stmt_temps(owned);
            return;
        }
        if (auto *pb = dynamic_cast<PubBlockStmt *>(s)) {
            for (auto &m: pb->statements) stmt(m.get());
            return;
        }
        if (auto *e = dynamic_cast<EmbedCStmt *>(s)) {
            c_funs_ << std::string(indent_ * 4, ' ') << e->code << "\n";
        }
    }

    void Codegen::emit_active_finally(bool exit_all, size_t loop_depth) {
        for (auto it = active_finally_.rbegin(); it != active_finally_.rend(); ++it) {
            if (!exit_all && it->loop_depth < loop_depth) continue;
            if (it->t && it->t->finally_body) stmt(it->t->finally_body.get());
        }
    }

    void Codegen::stmt_throw(const ThrowStmt *t) {
        // A throw inside a catch body escapes this try: run its finally first.
        // A throw inside a try body is caught by that try's catch, so its
        // finally runs on the normal catch path instead.
        for (auto it = active_finally_.rbegin(); it != active_finally_.rend(); ++it) {
            // The nearest enclosing try body will catch this throw, so its
            // finally (and outer ones) must not run yet. Only catch bodies
            // being escaped need their finally emitted here.
            if (!it->in_catch) break;
            if (it->t && it->t->finally_body) stmt(it->t->finally_body.get());
        }
        std::vector<std::string> temps = hoist_temp_str_args(t->value.get());
        emit("sinter_throw(" + expr(t->value.get()) + ", \"" + module_name_ + ".sinter\", " +
             std::to_string(t->line) + ");");
        finish_stmt_temps(temps);
    }

    void Codegen::stmt_try(const TryStmt *t) {
        const std::string frame = fresh("__exc");
        const std::string caught = fresh("__caught");
        emit("{");
        indent_++;
        emit("sinter_exc_frame " + frame + ";");
        emit(frame + ".prev = sinter_exc_top;");
        emit(frame + ".cleanup_top = sinter_cleanup_mark();");
        emit("sinter_exc_top = &" + frame + ";");
        emit("int " + caught + " = 0;");
        emit("if (setjmp(" + frame + ".buf) == 0) {");
        indent_++;
        try_depth_++;
        active_finally_.push_back({t, loop_scope_depths_.size(), false});
        stmt(t->body.get());
        active_finally_.pop_back();
        try_depth_--;
        indent_--;
        emit("} else { " + caught + " = 1; }");
        emit("sinter_exc_top = " + frame + ".prev;");
        if (t->catch_body) {
            emit("if (" + caught + ") {");
            indent_++;
            enter_scope();
            if (!t->error_name.empty()) {
                const std::string en = fresh(t->error_name);
                emit("sinter_string " + en + " = sinter_exc_take();");
                locals_[t->error_name] = en;
                local_types_[t->error_name] = "sinter_string";
                scopes_.back().push_back({t->error_name, en, "sinter_string", true, "string"});
                emit("sinter_cleanup_push(&" + en + ", sinter_cleanup_string);");
                scope_cleanup_counts_.back()++;
            }
            try_depth_++;
            active_finally_.push_back({t, loop_scope_depths_.size(), true});
            stmt(t->catch_body.get());
            active_finally_.pop_back();
            try_depth_--;
            leave_scope(false);
            indent_--;
            emit("}");
        }
        if (t->finally_body) {
            try_depth_++;
            stmt(t->finally_body.get());
            try_depth_--;
        }
        indent_--;
        emit("}");
    }

    void Codegen::stmt_var(const VarDeclStmt *v) {
        std::string t = v->type ? c_type(v->type.get()) : "long long";
        if (!v->type && v->init) {
            const auto inferred = c_type_of_expr(v->init.get());
            if (!inferred.empty()) t = inferred;
        }

        const std::string n = fresh(v->name);
        locals_[v->name] = n;
        local_types_[v->name] = t;
        if (v->type && v->type->is_ref) {
            ref_vars_.insert(v->name);
            if (v->type->kind == TypeAnnotation::Named && sem_.classes().contains(v->type->name))
                ref_class_vars_.insert(v->name);
        }

        if (auto *lam = dynamic_cast<LambdaExpr *>(v->init.get())) {
            // Lambda 变量：函数指针
            // 返回类型按表达式体推断（之前写死 long long，i64 lambda 会类型不符）
            auto &rp = lam->params;
            std::vector<std::string> ptypes;
            std::vector<std::string> hints;
            for (auto &pp: rp) {
                std::string pt = pp.type ? c_type(pp.type.get()) : std::string();
                hints.push_back(pt);
                ptypes.push_back(pt.empty() ? std::string("long long") : pt);
            }
            lambda_param_hints_pending_types_ = hints;   // 供无标注形参推断
            // expr_lambda 内部会先登记形参类型再推断返回类型
            const std::string fn = expr(lam);
            // 函数指针类型必须和实际生成的一致：用 lambda 体内推断出的返回类型，
            // 没有提示时再退回表达式静态推断
            std::string ret_t = "long long";
            {
                // 用 lambda 变量名去查语义层：这里改为直接按形参类型+表达式重算
                auto saved = locals_;
                auto saved_t = local_types_;
                for (size_t i = 0; i < lam->params.size(); ++i) {
                    locals_[lam->params[i].name] = Semantic::to_snake_ident(lam->params[i].name);
                    local_types_[lam->params[i].name] = ptypes[i];
                }
                if (!lam->is_block)
                    if (auto *e2 = std::get_if<ExprPtr>(&lam->body))
                        ret_t = c_type_of_expr(e2->get());
                locals_ = saved;
                local_types_ = saved_t;
            }
            std::string sig = ret_t + " (*" + n + ")(";
            bool firstp = true;
            for (auto &pt: ptypes) {
                if (!firstp) sig += ", ";
                firstp = false;
                sig += pt;
            }
            if (firstp) sig += "void";
            sig += ")";
            emit(sig + " = " + fn + ";");
            return;
        }
        // 数组类型 T[N]：降级为指向元素的指针，并自动分配 N 个元素。
        // Sinter 的数组是堆分配的，作用域结束不自动释放（与 str 一致）。
        if (v->type && v->type->kind == TypeAnnotation::Array) {
            const int n_elem = v->type->array_size;
            if (n_elem <= 0) {
                sem_.report_error("array length must be a positive integer: " + v->name);
                emit(t + " " + n + " = 0;");
                return;
            }
            if (v->init) {
                // 有初始化表达式：字面量按声明的元素类型生成（避免 int32_t 字面量撞 int64_t 声明），
                // 其它表达式（另一数组变量等）依旧直接赋值。
                if (auto *al = dynamic_cast<ArrayLiteralExpr *>(v->init.get())) {
                    std::string elem_c = t;
                    if (!elem_c.empty() && elem_c.back() == '*') elem_c.pop_back();
                    while (!elem_c.empty() && elem_c.back() == ' ') elem_c.pop_back();
                    // 数组是堆分配的 T*：字面量先写成 static 模板，再 memcpy 到堆上，
                    // 这样 free 合法，也避免字面量被当成堆指针去释放。
                    const std::string arr = expr_array_with(al, elem_c);
                    emit(t + " " + n + " = (" + t + ")malloc(sizeof(" + elem_c + ") * (size_t)" +
                         std::to_string(al->elements.size()) + ");");
                    emit("memcpy(" + n + ", " + arr + ", sizeof(" + elem_c + ") * (size_t)" +
                         std::to_string(al->elements.size()) + ");");
                } else {
                    emit(t + " " + n + " = " + upcast_if_needed(v->init.get(), t) + ";");
                }
            } else {
                // calloc 保证零初始化
                emit(t + " " + n + " = (" + t + ")calloc((size_t)" + std::to_string(n_elem) +
                     ", sizeof(*(" + t + ")0));");
            }
            // 元素带析构的数组：登记元素析构函数，作用域退出时逐个析构再 free。
            // （早期这里直接报错；现在支持了，元素按指针语义析构。）
            std::string elem_del;
            const TypeAnnotation *elem = v->type->inner_type.get();
            if (elem && elem->kind == TypeAnnotation::Named) {
                auto eci = sem_.classes().find(elem->name);
                if (eci != sem_.classes().end() && eci->second.has_destructor)
                    elem_del = eci->second.c_name + "_del";
            }
            // 内建数组的堆缓冲由自己持有，作用域退出时释放。
            // 只有「本语句自己分配」时才登记：
            //   - 无初始化 → 上面的 calloc；
            //   - 初始化是数组字面量 → 字面量本身也新建了一块堆缓冲。
            // 若初始化是另一个数组变量（借用别人的缓冲），登记会造成 double free。
            // 无初始化 → calloc；字面量 → 上面 malloc+memcpy 到堆。两者都由本语句持有。
            const bool self_owned = !v->init || dynamic_cast<ArrayLiteralExpr *>(v->init.get());
            if (self_owned && !scopes_.empty()) {
                ScopeVar sv;
                sv.name = v->name;
                sv.c_name = n;
                sv.type_c = t;
                sv.raii = true;
                sv.kind = "array";
                sv.elem_del = elem_del;
                sv.elem_count = n_elem;

                // Exception cleanup thunk: destroy elements (if needed) and free
                // the heap buffer before longjmp leaves the scope.
                std::string elem_c = t;
                while (!elem_c.empty() && elem_c.back() == '*') elem_c.pop_back();
                while (!elem_c.empty() && elem_c.back() == ' ') elem_c.pop_back();
                const std::string thunk = fresh("__arr_cleanup");
                pending_ << "static void " << thunk << "(void* p) { " << elem_c
                         << "* v = *(" << elem_c << "**)p; if (v) { ";
                if (!elem_del.empty()) {
                    pending_ << "for (int64_t i = " << n_elem << " - 1; i >= 0; --i) {"
                             << " if (v[i]) { " << elem_del << "(v[i]); v[i] = 0; } } ";
                }
                pending_ << "free(v); *(" << elem_c << "**)p = 0; } }\n";
                emit("sinter_cleanup_push(&" + n + ", " + thunk + ");");
                scope_cleanup_counts_.back()++;

                scopes_.back().push_back(std::move(sv));
            }
            return;
        }

        // 初始化式里「作为实参传出去的临时字符串」先落地，赋值完立即释放。
        // 只提升实参，不提升初始化式本身——后者绑定到变量，由 RAII 接管。
        // 数组分支已在上面的 return 里返回：元素是 memcpy 的指针副本，
        // 提升后释放会给数组留下悬空指针。
        std::vector<std::string> temps = hoist_temp_str_args(v->init.get());
        if (v->init)
            emit(t + " " + n + " = " + upcast_if_needed(v->init.get(), t) + ";");
        else emit(t + " " + n + " = 0;");
        finish_stmt_temps(temps);

        // RAII 登记：按最终生效的 C 类型判断，
        // 因此 var r = Res(1)（类型由初始化表达式推断）与 var r: Res 一样会登记。
        register_raii(v->name, n, t);
    }

    void Codegen::stmt_destructure(const DestructureStmt *d) {
        // 解构的初始化式里作为实参的临时字符串：先落地，取出字段后释放。
        std::vector<std::string> temps = hoist_temp_str_args(d->init.get());
        const std::string tmp = fresh("__ds");
        const std::string ty = d->init ? c_type_of_expr(d->init.get()) : "long long";
        emit(ty + " " + tmp + " = " + expr(d->init.get()) + ";");
        finish_stmt_temps(temps);

        std::vector<std::pair<std::string, std::string> > fields; // name, type
        for (auto &[cname, ci]: sem_.classes()) {
            if ("struct " + ci.c_name + "*" == ty || "struct " + ci.c_name == ty) {
                for (auto &f: ci.fields) fields.emplace_back(f.c_name, f.type_c);
                break;
            }
        }
        for (size_t i = 0; i < d->targets.size(); ++i) {
            const auto &t = d->targets[i];
            if (t.mode == DestructureStmt::Target::Ignore) continue;
            std::string fld = i < fields.size() ? fields[i].first : ("f" + std::to_string(i));
            std::string ft = t.type ? c_type(t.type.get()) : (i < fields.size() ? fields[i].second : "long long");
            const std::string n = fresh(t.name);
            const std::string acc = ty.ends_with('*') ? (tmp + "->" + fld) : (tmp + "." + fld);
            emit(ft + " " + n + " = " + acc + ";");
            locals_[t.name] = n;
            local_types_[t.name] = ft;
        }
    }

    bool Codegen::emit_dispatch_return(ReturnStmt *r) {
        if (!r->value || cur_type_params_.empty()) return false;
        auto *c = dynamic_cast<CallExpr *>(r->value.get());
        if (!c) return false;
        auto *id = dynamic_cast<IdentifierExpr *>(c->callee.get());
        if (!id) return false;
        const std::string generic = id->name;
        static const std::vector<std::string> kFamilies = {
            "sinter_gen_clone", "sinter_gen_free", "sinter_gen_drop", "sinter_gen_grow",
            "sinter_gen_hash", "sinter_gen_cmp", "sinter_gen_blank",
        };
        bool known = false;
        for (auto &fam: kFamilies) {
            if (generic == fam ||
                (generic.size() > fam.size() && generic.compare(0, fam.size(), fam) == 0 &&
                 generic[generic.size() == fam.size() ? 0 : fam.size()] == '_')) {
                known = true;
                break;
            }
        }
        if (!known || c->args.empty()) return false;

        // 条件里要判的是「与 T 对应的那个形参所接收的实参」是不是 T 本身（而不是 *T）。
        // `grow(data, n)` 里 T 是 data 的元素类型，判的是 data 是否为 null；
        // `clone(v, fallback)` 里 T 就是 v 的类型，直接判 v。
        size_t argi = 0;
        if (generic == "sinter_gen_grow") {
            argi = 0;
        } else {
            for (size_t i = 0; i < c->args.size(); ++i) {
                if (!c->args[i].value) continue;
                const std::string at = c_type_of_expr(c->args[i].value.get());
                // 判「不是指针」：*T 缩一级之后变成指针，T 本身不是
                if (!at.empty() && at.back() != '*') { argi = i; break; }
            }
        }
        if (argi >= c->args.size() || !c->args[argi].value) return false;
        const std::string probe = expr(c->args[argi].value.get());

        const bool was_indent = indent_ == 1;
        (void)was_indent;
        std::ostringstream chain;
        bool first = true;
        for (size_t i = 0; i < cur_type_params_.size() && i < cur_type_args_.size(); ++i) {
            if (cur_type_params_[i].is_pack) continue;
            const std::string cs = generic_dispatch_name_at(generic, i);
            if (cs.empty()) continue;
            chain << (first ? "" : " else ") << "if (" << probe << " != NULL) return " << cs << "(";
            for (size_t a = 0; a < c->args.size(); ++a) {
                if (a) chain << ", ";
                chain << expr(c->args[a].value.get());
            }
            chain << ");";
            first = false;
        }
        if (first) return false;
        emit("{");
        ++indent_;
        emit(chain.str());
        emit("return " + probe + ";");
        --indent_;
        emit("}");
        return true;
    }

    void Codegen::stmt_return(const ReturnStmt *r) {
        emit_active_finally(true, 0);
        // 类型参数分派助记符：`return grow(data, n)` 这类要靠当前类实例的类型实参
        // 才能定下目标函数的写法，在这里整体改写成按类型分派的 if 链。
        if (emit_dispatch_return(const_cast<ReturnStmt *>(r))) return;
        // void 函数的 `=>` 表达式体：`fun dump(x: i64): void => printlnInt(x)`。
        //
        // 解析器把 `=> expr` 统一包成 `return expr;`，但 void 函数里这个 return
        // 是类型错误的：生成 `void __ret = expr;` 会得到
        // `error: variable or field '__ret' declared void`，
        // 而 `return expr;` 在 ISO C 里是明令禁止的（gcc 只把带值返回当扩展）。
        // 正确形态就是「表达式语句 + 补析构 + return;」——expr 只求值、不保留。
        if (current_return_type_ == "void") {
            std::vector<std::string> owned;
            if (r->value) {
                owned = hoist_temp_str_args(r->value.get());
                if (c_type_of_expr(r->value.get()) == "sinter_string") {
                    // 丢弃的字符串结果同样要有人释放，否则泄漏。
                    const std::string t = fresh("__tmpstr");
                    emit("sinter_string " + t + " = " + expr(r->value.get()) + ";");
                    owned.push_back(t);
                } else {
                    const std::string x = expr(r->value.get());
                    if (!x.empty()) emit(x + ";");
                }
            }
            emit_scope_exits(fn_scope_depth_, fn_no_raii_);
            finish_stmt_temps(owned);
            emit("return;");
            return;
        }
        // 顺序很关键：return 表达式可能引用即将被析构的局部对象，
        // 因此必须先把值算到临时变量，再做清理，最后返回临时变量。
        if (r->value) {
            // 返回表达式里作为「实参」的临时字符串：先落地，值算完立刻释放。
            // 返回表达式本身不提升——它的所有权要移交给调用方。
            std::vector<std::string> temps = hoist_temp_str_args(r->value.get());
            // 返回子类对象而声明返回基类 → 自动取内嵌 __base 子对象
            const std::string val = upcast_if_needed(r->value.get(), current_return_type_);
            const bool ret_str = current_return_type_ == "sinter_string";
            const bool need_cleanup = !fn_no_raii_ && scopes_.size() > fn_scope_depth_;
            // 有临时值要释放时也必须先落到 __ret：`return expr;` 之后没地方插语句。
            if (need_cleanup || !temps.empty()) {
                const std::string tmp = fresh("__ret");
                const std::string ty = current_return_type_.empty()
                                           ? std::string("long long")
                                           : current_return_type_;
                emit(ty + " " + tmp + " = " + val + ";");
                // 字符串返回值把所有权交给调用方：被返回的那个局部变量必须
                // 从清理表中摘除，否则调用方拿到的是已释放的缓冲区。
                if (ret_str) release_returned_string(r->value.get());
                // return 会跳过所有内层 leave_scope，必须在这里补上析构
                if (need_cleanup) emit_scope_exits(fn_scope_depth_, false);
                finish_stmt_temps(temps);
                emit("return " + tmp + ";");
            } else {
                emit("return " + val + ";");
            }
        } else {
            emit_scope_exits(fn_scope_depth_, fn_no_raii_);
            emit("return;");
        }
    }

    /// `return s`（s 是 sinter_string 局部变量）时，把 s 的所有权移交给调用方：
    /// 从各个作用域的清理表里摘除它，避免返回后立刻被 free。
    void Codegen::release_returned_string(const Expression *e) {
        auto *id = dynamic_cast<const IdentifierExpr *>(e);
        if (!id) return;
        const auto lit = locals_.find(id->name);
        if (lit == locals_.end()) return;
        for (auto &sc: scopes_) {
            for (auto &sv: sc) {
                if (sv.c_name == lit->second && sv.raii) {
                    sv.raii = false;
                    return;
                }
            }
        }
    }

    /// 发射「每轮重新求值」的循环条件块，返回条件变量名。形如：
    ///     sinter_string __tmpstr = ...;        // 提升出来的临时字符串
    ///     long long __cond = (a < len(b)) ? 1 : 0;
    ///     sinter_string_free(__tmpstr); ...
    ///     if (!__cond) break;
    /// 之所以要单独成块：循环条件不能提到循环外面，否则每轮读到的是第一轮的值。
    std::string Codegen::emit_loop_cond_block(Expression *cond) {
        std::vector<std::string> temps = hoist_temp_str_args(cond);
        const std::string c = fresh("__cond");
        emit("long long " + c + " = (" + expr(cond) + ") ? 1 : 0;");
        finish_stmt_temps(temps);
        emit("if (!" + c + ") break;");
        return c;
    }

    void Codegen::stmt_if(const IfStmt *i) {
        // 条件里的临时字符串先落地：声明自然落在 `if (` 之前，
        // 条件只求值一次，因此可以在整个 if/else 结束之后再释放。
        std::vector<std::string> temps = hoist_temp_str_args(i->cond.get());
        emit("if (" + expr(i->cond.get()) + ") {");
        indent_++;
        enter_scope();
        stmt(i->then_b.get());
        leave_scope(false);
        indent_--;
        if (i->else_b) {
            emit("} else {");
            indent_++;
            enter_scope();
            stmt(i->else_b.get());
            leave_scope(false);
            indent_--;
        }
        emit("}");
        finish_stmt_temps(temps);
    }

    void Codegen::stmt_while(const WhileStmt *w) {
        if (needs_hoist(w->cond.get())) {
            // 条件里要释放的临时字符串必须每轮重新生成，不能提到循环外面，
            // 因此改写成 for(;;) + 循环体顶部的条件块。
            // `continue` 在 C 的 for 里本来就跳回循环顶部，语义与 while 一致。
            emit("for (;;) {");
            indent_++;
            enter_scope();
            loop_scope_depths_.push_back(scopes_.size() - 1);
            loop_continue_labels_.push_back(std::string());
            emit_loop_cond_block(w->cond.get());
            stmt(w->body.get());
            loop_continue_labels_.pop_back();
            loop_scope_depths_.pop_back();
            leave_scope(false);
            indent_--;
            emit("}");
            return;
        }
        emit("while (" + expr(w->cond.get()) + ") {");
        indent_++;
        enter_scope();
        loop_scope_depths_.push_back(scopes_.size() - 1);
        loop_continue_labels_.push_back(std::string());
        stmt(w->body.get());
        loop_continue_labels_.pop_back();
        loop_scope_depths_.pop_back();
        leave_scope(false);
        indent_--;
        emit("}");
    }

    void Codegen::stmt_for(const ForStmt *f) {
        const std::string var = f->variables.empty() ? "i" : f->variables[0];

        if (auto *r = dynamic_cast<RangeExpr *>(f->iterable.get())) {
            const std::string v = fresh(var);
            locals_[var] = v;
            local_types_[var] = "long long";
            const std::string op = r->inclusive ? "<=" : "<";
            // 下界只求值一次，可以外提；上界每轮都要重算，需要提升时改写成 for(;;)。
            std::vector<std::string> lo = hoist_temp_str_args(r->start.get());
            std::string lo_code = expr(r->start.get());
            if (!lo.empty()) {
                // 先把下界落到一个局部变量，再释放临时值：否则 free 会落在
                // `for (...) {` 之后，变成「每轮执行一次」，位置也不对。
                const std::string lv = fresh("__lo");
                emit("long long " + lv + " = " + lo_code + ";");
                finish_stmt_temps(lo);
                lo.clear();
                lo_code = lv;
            }
            if (!needs_hoist(r->end.get())) {
                emit("for (long long " + v + " = " + lo_code + "; " + v + " " + op + " " +
                     expr(r->end.get()) + "; ++" + v + ") {");
                finish_stmt_temps(lo);
                indent_++;
                enter_scope();
                loop_scope_depths_.push_back(scopes_.size() - 1);
                loop_continue_labels_.push_back(std::string());
                stmt(f->body.get());
                loop_continue_labels_.pop_back();
                loop_scope_depths_.pop_back();
                leave_scope(false);
                indent_--;
                emit("}");
                return;
            }
            emit("for (long long " + v + " = " + lo_code + ";; ++" + v + ") {");
            finish_stmt_temps(lo);
            indent_++;
            enter_scope();
            loop_scope_depths_.push_back(scopes_.size() - 1);
            loop_continue_labels_.push_back(std::string());
            {
                std::vector<std::string> hi = hoist_temp_str_args(r->end.get());
                const std::string c = fresh("__cond");
                emit("long long " + c + " = (" + v + " " + op + " " + expr(r->end.get()) +
                     ") ? 1 : 0;");
                finish_stmt_temps(hi);
                emit("if (!" + c + ") break;");
            }
            stmt(f->body.get());
            loop_continue_labels_.pop_back();
            loop_scope_depths_.pop_back();
            leave_scope(false);
            indent_--;
            emit("}");
            return;
        }
        if (auto *c = dynamic_cast<CallExpr *>(f->iterable.get())) {
            if (auto *id = dynamic_cast<IdentifierExpr *>(c->callee.get());
                id && id->name == "range" && c->args.size() == 2) {
                const std::string v = fresh(var);
                locals_[var] = v;
                local_types_[var] = "long long";
                std::vector<std::string> lo = hoist_temp_str_args(c->args[0].value.get());
                std::string lo_code = expr(c->args[0].value.get());
                if (!lo.empty()) {
                    // 同 RangeExpr：下界只在循环开始前求值一次
                    const std::string lv = fresh("__lo");
                    emit("long long " + lv + " = " + lo_code + ";");
                    finish_stmt_temps(lo);
                    lo.clear();
                    lo_code = lv;
                }
                if (!needs_hoist(c->args[1].value.get())) {
                    emit("for (long long " + v + " = " + lo_code + "; " + v + " < " +
                         expr(c->args[1].value.get()) + "; ++" + v + ") {");
                    finish_stmt_temps(lo);
                    indent_++;
                    enter_scope();
                    loop_scope_depths_.push_back(scopes_.size() - 1);
                    loop_continue_labels_.push_back(std::string());
                    stmt(f->body.get());
                    loop_continue_labels_.pop_back();
                    loop_scope_depths_.pop_back();
                    leave_scope(false);
                    indent_--;
                    emit("}");
                    return;
                }
                emit("for (long long " + v + " = " + lo_code + ";; ++" + v + ") {");
                finish_stmt_temps(lo);
                indent_++;
                enter_scope();
                loop_scope_depths_.push_back(scopes_.size() - 1);
                loop_continue_labels_.push_back(std::string());
                {
                    std::vector<std::string> hi = hoist_temp_str_args(c->args[1].value.get());
                    const std::string cv = fresh("__cond");
                    emit("long long " + cv + " = (" + v + " < " +
                         expr(c->args[1].value.get()) + ") ? 1 : 0;");
                    finish_stmt_temps(hi);
                    emit("if (!" + cv + ") break;");
                }
                stmt(f->body.get());
                loop_continue_labels_.pop_back();
                loop_scope_depths_.pop_back();
                leave_scope(false);
                indent_--;
                emit("}");
                return;
            }
        }
        emit("/* unsupported for-iterable */");
    }

    void Codegen::stmt_do_while(const DoWhileStmt *d) {
        // do-while 的条件在每轮末尾求值。条件里有需要释放的临时字符串时，
        // 把条件求值挪到循环体末尾的一段代码，并让 `continue` 用 goto 跳到它前面
        // （C 的 continue 在 do-while 里本来就跳到条件处，语义一致）。
        const bool hoist_cond = needs_hoist(d->cond.get());
        const std::string cvar = hoist_cond ? fresh("__cond") : std::string();
        const bool need_label = hoist_cond && has_continue(d->body.get());
        const std::string label = need_label ? fresh("__cond_label") : std::string();
        if (hoist_cond) emit("long long " + cvar + ";");
        emit("do {");
        indent_++;
        enter_scope();
        loop_scope_depths_.push_back(scopes_.size() - 1);
        loop_continue_labels_.push_back(label);
        stmt(d->body.get());
        loop_continue_labels_.pop_back();
        loop_scope_depths_.pop_back();
        leave_scope(false);
        if (hoist_cond) {
            if (need_label) emit(label + ": ;");
            std::vector<std::string> temps = hoist_temp_str_args(d->cond.get());
            emit(cvar + " = (" + expr(d->cond.get()) + ") ? 1 : 0;");
            finish_stmt_temps(temps);
        }
        indent_--;
        emit(hoist_cond ? "} while (" + cvar + ");" : "} while (" + expr(d->cond.get()) + ");");
    }

    /// 该语句子树里（不进入嵌套循环）是否有 `continue`。
    /// 只用来决定 do-while 要不要发 goto 标签：没 continue 时发标签会被 -Wunused-label 抓。
    bool Codegen::has_continue(const Statement *s) {
        if (!s) return false;
        if (dynamic_cast<const ContinueStmt *>(s)) return true;
        // 内层循环的 continue 归内层管
        if (dynamic_cast<const WhileStmt *>(s) || dynamic_cast<const ForStmt *>(s) ||
            dynamic_cast<const DoWhileStmt *>(s))
            return false;
        if (auto *b = dynamic_cast<const BlockStmt *>(s)) {
            for (auto &st: b->statements)
                if (has_continue(st.get())) return true;
            return false;
        }
        if (auto *i = dynamic_cast<const IfStmt *>(s))
            return has_continue(i->then_b.get()) || has_continue(i->else_b.get());
        if (auto *p = dynamic_cast<const PubBlockStmt *>(s)) {
            for (auto &st: p->statements)
                if (has_continue(st.get())) return true;
            return false;
        }
        return false;
    }

    void Codegen::stmt_block(const BlockStmt *b) {
        enter_scope();
        for (auto &s: b->statements) stmt(s.get());
        leave_scope(b->no_raii);
    }

    void Codegen::stmt_del(const DelStmt *d) {
        auto *id = dynamic_cast<IdentifierExpr *>(d->target.get());
        if (!id) { emit("/* del: unsupported target */"); return; }
        const auto it = locals_.find(id->name);
        if (it == locals_.end()) return;
        const std::string cname = it->second;

        std::string del_class;
        const auto ti = local_types_.find(id->name);
        if (ti != local_types_.end()) {
            if (const ClassInfo *dci = class_by_c_type(ti->second)) del_class = dci->c_name;
        }
        if (del_class.empty()) { emit(cname + " = 0;"); return; }
        emit("if (" + cname + ") { " + del_class + "_del(" + cname + "); " + cname + " = 0; }");
        for (auto &sc: scopes_)
            for (auto &sv: sc)
                if (sv.c_name == cname) sv.raii = false;
    }
} // namespace sinter