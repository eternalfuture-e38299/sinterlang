// sinter front: 泛型类 / 泛型结构体单态化实现
//
// 做法：在语义分析之前做一遍 AST 变换（类似 C++ 模板实例化）。
//   - 收集所有带 <T> 的 class/struct（模板）
//   - 把每个使用处的 `Box<i64>` 改写成 `Box_i64`，并记录一条实例化需求
//   - 对每条需求，把模板深拷贝一份：替换类型参数、改名字、再（递归地）改写内部
//     的类型与构造调用，最后插入到「声明模板的那个模块」的顶层语句里
//   - 模板本身从 AST 中删除，后续语义/代码生成完全按普通类处理
//
// 这样泛型类与泛型结构体不需要语义层和代码生成层做任何特殊处理：单态化之后
// 就是普通类，RAII、继承、虚表、复制/移动构造、跨模块导出都自动可用。
//
// 设计决策（与泛型函数保持一致）：
//   1) 使用处必须显式书写类型实参：`Box<i64>` / `Box<i64>(...)`。写 `Box()` 或
//      `val b: Box` 会报错并指出缺少哪个类型参数。
//   2) `<T must number>` 在实例化时严格校验，违反即报错且不生成实例。
//   3) 支持模板内部自引用（`Node<T>` 里的 `field: Node<T>`），改写为 `Node_T`。
#include "front/generic_mono.hpp"

#include <algorithm>
#include <set>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sinter {
    // ==========================================================================
    // GenericTable：实例名 ↔ 模板名 + 类型实参
    // ==========================================================================
    void GenericTable::add(const std::string &inst, const std::vector<TemplateParam> &tparams,
                           const std::string &module_name) {
        std::string short_name = inst;
        const auto dot = short_name.rfind('.');
        if (dot != std::string::npos) short_name = short_name.substr(dot + 1);
        items_[short_name] = Item{tparams, module_name};
    }

    std::string GenericTable::compose(const std::string &tmpl,
                                      const std::vector<std::string> &args) {
        std::string out = tmpl;
        for (auto &a: args) out += "_" + a;
        return out;
    }

    bool GenericTable::split(const std::string &inst_in, std::vector<std::string> &args) const {
        std::string inst = inst_in;
        const auto dot = inst.rfind('.');
        if (dot != std::string::npos) inst = inst.substr(dot + 1);
        const auto it = items_.find(inst);
        if (it == items_.end()) return false;
        // 后缀按 `_` 切段；末尾 n 段对应 n类型参数，前面剩余段拼回模板名
        std::vector<std::string> parts;
        std::string cur;
        for (char ch: inst) {
            if (ch == '_') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        parts.push_back(cur);
        const size_t n = it->second.tparams.size();
        if (n == 0 || parts.size() <= n) return false;
        args.assign(parts.end() - static_cast<long>(n), parts.end());
        return true;
    }

    // ==========================================================================
    // 泛型类 / 泛型结构体单态化
    // ==========================================================================
    namespace {
        /// 类型参数替换表：T → 具体类型
        using Subst = std::unordered_map<std::string, std::unique_ptr<TypeAnnotation> >;

        /// 模板类信息
        struct TemplateInfo {
            ClassDecl *decl = nullptr;
            size_t module_index = 0;
        };

        /// 一条实例化需求
        struct Request {
            std::string tmpl_name;   // 模板的 Sinter 名：Box
            std::string inst_name;   // 实例的 Sinter 名：Box_i64
            std::vector<std::unique_ptr<TypeAnnotation> > args;
        };

        struct Context {
            std::vector<GenericModule> *modules = nullptr;
            GenericTable *table = nullptr;
            std::unordered_map<std::string, TemplateInfo> templates;
            /// 已落地的实例：inst_name → 模块下标
            std::unordered_map<std::string, size_t> materialized;
            std::vector<Request> pending;
            std::string error;
            /// 跨模块使用：模块下标 → 需要补的 import 模块名
            std::unordered_map<size_t, std::vector<std::string> > imports;
        };

        // ==========================================================================
        // 类型名（Sinter 侧实例名）：Box<i64> → Box_i64，Pair<i64, str> → Pair_i64_str
        // ==========================================================================
        std::string mangle_type(const TypeAnnotation *t) {
            if (!t) return "void";
            switch (t->kind) {
                case TypeAnnotation::Named: return t->name;
                case TypeAnnotation::Generic: {
                    std::string r = t->name;
                    for (auto &a: t->generic_args) r += "_" + mangle_type(a.get());
                    return r;
                }
                case TypeAnnotation::Pointer: return mangle_type(t->inner_type.get()) + "_ptr";
                case TypeAnnotation::Optional: return mangle_type(t->inner_type.get()) + "_opt";
                case TypeAnnotation::Array: return mangle_type(t->inner_type.get()) + "_arr";
                case TypeAnnotation::FunctionType: return "fn";
            }
            return "void";
        }

        // ==========================================================================
        // 类型替换（深拷贝 + 代入 T）
        // ==========================================================================
        std::unique_ptr<TypeAnnotation> subst_type(const TypeAnnotation *t, const Subst &subst) {
            if (!t) return nullptr;
            if (t->kind == TypeAnnotation::Named) {
                const auto it = subst.find(t->name);
                if (it != subst.end()) {
                    auto c = clone_type(it->second.get());
                    // 保留使用处的引用修饰（T& / T&& / const T&）
                    c->is_const_ref = t->is_const_ref;
                    c->is_ref = t->is_ref;
                    c->is_rvalue_ref = t->is_rvalue_ref;
                    return c;
                }
            }
            auto c = std::make_unique<TypeAnnotation>();
            c->kind = t->kind;
            c->name = t->name;
            c->array_size = t->array_size;
            c->is_const_ref = t->is_const_ref;
            c->is_ref = t->is_ref;
            c->is_rvalue_ref = t->is_rvalue_ref;
            if (t->inner_type) c->inner_type = subst_type(t->inner_type.get(), subst);
            for (auto &g: t->generic_args) c->generic_args.push_back(subst_type(g.get(), subst));
            return c;
        }

        ExprPtr clone_expr_subst(const Expression *e, const Subst &subst);
        StmtPtr clone_stmt_subst(const Statement *s, const Subst &subst);
        ExprPtr clone_expr_subst_impl(const Expression *e, const Subst &subst);
        StmtPtr clone_stmt_subst_impl(const Statement *s, const Subst &subst);

        /// 克隆实例体时保留源码位置。
        ///
        /// 泛型实例是「模板体的副本」，位置必须仍指向模板源码：
        ///   - 诊断能指到真正写了这行代码的地方，而不是 0:0；
        ///   - 第二遍 comptime pass 折 `__LINE__` / `__COLUMN__` 时才有位置可折。
        ExprPtr clone_expr_subst(const Expression *e, const Subst &subst) {
            auto c = clone_expr_subst_impl(e, subst);
            if (c) {
                c->line = e->line;
                c->col = e->col;
            }
            return c;
        }

        StmtPtr clone_stmt_subst(const Statement *s, const Subst &subst) {
            auto c = clone_stmt_subst_impl(s, subst);
            if (c) {
                c->line = s->line;
                c->col = s->col;
            }
            return c;
        }

        Parameter clone_param_subst(const Parameter &p, const Subst &subst) {
            Parameter r;
            r.name = p.name;
            r.is_val = p.is_val;
            r.is_var = p.is_var;
            r.is_self = p.is_self;
            r.type = subst_type(p.type.get(), subst);
            r.default_value = clone_expr_subst(p.default_value.get(), subst);
            return r;
        }

        /// 把 FunctionDecl 的克隆结果从 StmtPtr 转成 unique_ptr<FunctionDecl>
        std::unique_ptr<FunctionDecl> as_fn(StmtPtr sp) {
            if (!sp) return nullptr;
            auto *raw = dynamic_cast<FunctionDecl *>(sp.release());
            return std::unique_ptr<FunctionDecl>(raw);
        }

        // ==========================================================================
        // 表达式深拷贝（同时代入类型参数）
        // ==========================================================================
        ExprPtr clone_expr_subst_impl(const Expression *e, const Subst &subst) {
            if (!e) return nullptr;
            switch (e->kind) {
                case Expression::Literal: {
                    auto *l = static_cast<const LiteralExpr *>(e);
                    auto c = std::make_unique<LiteralExpr>(l->lit, l->raw);
                    c->value = l->value;
                    c->suffix = l->suffix;
                    return c;
                }
                case Expression::Identifier:
                    // 标识符是值位置的名字（变量 / 函数 / 构造调用目标）。
                    // 类型参数 T 只出现在类型位置，这里不做替换；用到 T 的具体
                    // 类型已在 subst_type 里代入，因此构造调用 `Box<i64>(...)`
                    // 的 callee 名字会在改写阶段（Rewriter）统一处理。
                    return std::make_unique<IdentifierExpr>(static_cast<const IdentifierExpr *>(e)->name);
                case Expression::Builtin:
                    // 编译期位置关键字：原样复制，等第二遍 comptime pass 折叠。
                    // 这里必须显式处理——否则会落到函数末尾的 nullptr，
                    // 泛型实例体里的 `__LINE__` 会把整条表达式变成空指针。
                    return std::make_unique<BuiltinExpr>(static_cast<const BuiltinExpr *>(e)->which);
                case Expression::Binary: {
                    auto *b = static_cast<const BinaryExpr *>(e);
                    return std::make_unique<BinaryExpr>(b->op, clone_expr_subst(b->left.get(), subst),
                                                        clone_expr_subst(b->right.get(), subst));
                }
                case Expression::Unary: {
                    auto *u = static_cast<const UnaryExpr *>(e);
                    return std::make_unique<UnaryExpr>(u->op, clone_expr_subst(u->operand.get(), subst));
                }
                case Expression::Call: {
                    auto *c = static_cast<const CallExpr *>(e);
                    auto n = std::make_unique<CallExpr>();
                    n->callee = clone_expr_subst(c->callee.get(), subst);
                    for (auto &a: c->args)
                        n->args.emplace_back(a.name, clone_expr_subst(a.value.get(), subst));
                    for (auto &a: c->arguments)
                        n->arguments.push_back(clone_expr_subst(a.get(), subst));
                    for (auto &t: c->type_args) n->type_args.push_back(subst_type(t.get(), subst));
                    return n;
                }
                case Expression::MemberAccess: {
                    auto *m = static_cast<const MemberAccessExpr *>(e);
                    auto n = std::make_unique<MemberAccessExpr>(clone_expr_subst(m->object.get(), subst), m->member);
                    n->null_safe = m->null_safe;
                    return n;
                }
                case Expression::IndexAccess: {
                    auto *i = static_cast<const IndexAccessExpr *>(e);
                    return std::make_unique<IndexAccessExpr>(clone_expr_subst(i->object.get(), subst),
                                                             clone_expr_subst(i->index.get(), subst));
                }
                case Expression::Assignment: {
                    auto *a = static_cast<const AssignmentExpr *>(e);
                    auto n = std::make_unique<AssignmentExpr>(clone_expr_subst(a->target.get(), subst),
                                                              clone_expr_subst(a->value.get(), subst));
                    n->op = a->op;
                    return n;
                }
                case Expression::Pipeline: {
                    auto *p = static_cast<const PipelineExpr *>(e);
                    return std::make_unique<PipelineExpr>(clone_expr_subst(p->left.get(), subst),
                                                          clone_expr_subst(p->right.get(), subst));
                }
                case Expression::Lambda: {
                    auto *l = static_cast<const LambdaExpr *>(e);
                    auto n = std::make_unique<LambdaExpr>();
                    for (auto &p: l->params) n->params.push_back(clone_param_subst(p, subst));
                    n->is_block = l->is_block;
                    if (l->is_block) {
                        if (auto *b = std::get_if<StmtPtr>(&l->body))
                            n->body = clone_stmt_subst(b->get(), subst);
                    } else if (auto *ex = std::get_if<ExprPtr>(&l->body)) {
                        n->body = clone_expr_subst(ex->get(), subst);
                    }
                    return n;
                }
                case Expression::Ternary: {
                    auto *t = static_cast<const TernaryExpr *>(e);
                    auto n = std::make_unique<TernaryExpr>(clone_expr_subst(t->cond.get(), subst),
                                                           clone_expr_subst(t->then_b.get(), subst),
                                                           clone_expr_subst(t->else_b.get(), subst));
                    n->is_elvis = t->is_elvis;
                    return n;
                }
                case Expression::Range: {
                    auto *r = static_cast<const RangeExpr *>(e);
                    return std::make_unique<RangeExpr>(clone_expr_subst(r->start.get(), subst),
                                                       clone_expr_subst(r->end.get(), subst), r->inclusive);
                }
                case Expression::Cast: {
                    auto *c = static_cast<const CastExpr *>(e);
                    return std::make_unique<CastExpr>(clone_expr_subst(c->expr.get(), subst),
                                                      subst_type(c->target.get(), subst));
                }
                case Expression::Grouping:
                    return std::make_unique<GroupingExpr>(
                        clone_expr_subst(static_cast<const GroupingExpr *>(e)->expr.get(), subst));
                case Expression::IfExpr: {
                    auto *i = static_cast<const IfExpr *>(e);
                    return std::make_unique<IfExpr>(clone_expr_subst(i->cond.get(), subst),
                                                    clone_expr_subst(i->then_b.get(), subst),
                                                    clone_expr_subst(i->else_b.get(), subst));
                }
                case Expression::WhenExpr: {
                    auto *w = static_cast<const WhenExpr *>(e);
                    auto n = std::make_unique<WhenExpr>();
                    n->subject = clone_expr_subst(w->subject.get(), subst);
                    for (auto &br: w->branches) {
                        WhenExpr::Branch nb;
                        for (auto &pt: br.patterns) {
                            WhenExpr::Pattern np;
                            np.mode = pt.mode;
                            np.value = clone_expr_subst(pt.value.get(), subst);
                            np.lo = clone_expr_subst(pt.lo.get(), subst);
                            np.hi = clone_expr_subst(pt.hi.get(), subst);
                            np.type_name = pt.type_name;
                            np.inclusive = pt.inclusive;
                            nb.patterns.push_back(std::move(np));
                        }
                        nb.body = clone_expr_subst(br.body.get(), subst);
                        nb.body_block = clone_stmt_subst(br.body_block.get(), subst);
                        n->branches.push_back(std::move(nb));
                    }
                    n->else_branch = clone_expr_subst(w->else_branch.get(), subst);
                    n->else_block = clone_stmt_subst(w->else_block.get(), subst);
                    return n;
                }
                case Expression::ArrayLiteral: {
                    auto *a = static_cast<const ArrayLiteralExpr *>(e);
                    auto n = std::make_unique<ArrayLiteralExpr>();
                    for (auto &x: a->elements) n->elements.push_back(clone_expr_subst(x.get(), subst));
                    return n;
                }
                case Expression::StringTemplate: {
                    auto *s = static_cast<const StringTemplateExpr *>(e);
                    auto n = std::make_unique<StringTemplateExpr>();
                    for (auto &pt: s->parts) {
                        if (pt.expr) n->parts.emplace_back(clone_expr_subst(pt.expr.get(), subst));
                        else n->parts.emplace_back(pt.text);
                    }
                    return n;
                }
                case Expression::ScopeCall: {
                    auto *s = static_cast<const ScopeCallExpr *>(e);
                    auto n = std::make_unique<ScopeCallExpr>();
                    n->kind_sc = s->kind_sc;
                    n->member_name = s->member_name;
                    n->it_name = s->it_name;
                    n->receiver = clone_expr_subst(s->receiver.get(), subst);
                    n->body_expr = clone_expr_subst(s->body_expr.get(), subst);
                    n->body = clone_stmt_subst(s->body.get(), subst);
                    return n;
                }
                case Expression::BlockExprKind: {
                    auto *b = static_cast<const BlockExpr *>(e);
                    auto n = std::make_unique<BlockExpr>();
                    n->is_block = b->is_block;
                    n->result = clone_expr_subst(b->result.get(), subst);
                    n->block = clone_stmt_subst(b->block.get(), subst);
                    return n;
                }
            }
            return nullptr;
        }

        // ==========================================================================
        // 语句深拷贝：逐类对照 ast.hpp 的 Statement::Kind 全量覆盖
        // ==========================================================================
        StmtPtr clone_stmt_subst_impl(const Statement *s, const Subst &subst) {
            if (!s) return nullptr;
            switch (s->kind) {
                case Statement::Block: {
                    auto *b = static_cast<const BlockStmt *>(s);
                    auto n = std::make_unique<BlockStmt>();
                    n->no_raii = b->no_raii;
                    for (auto &x: b->statements) n->statements.push_back(clone_stmt_subst(x.get(), subst));
                    return n;
                }
                case Statement::VarDecl: {
                    auto *v = static_cast<const VarDeclStmt *>(s);
                    auto n = std::make_unique<VarDeclStmt>();
                    n->name = v->name;
                    n->type = subst_type(v->type.get(), subst);
                    n->init = clone_expr_subst(v->init.get(), subst);
                    n->is_mutable = v->is_mutable;
                    n->is_const = v->is_const;
                    n->is_static = v->is_static;
                    n->is_pub = v->is_pub;
                    n->is_raw_c = v->is_raw_c;
                    return n;
                }
                case Statement::Destructure: {
                    auto *d = static_cast<const DestructureStmt *>(s);
                    auto n = std::make_unique<DestructureStmt>();
                    for (auto &t: d->targets) {
                        DestructureStmt::Target nt;
                        nt.mode = t.mode;
                        nt.name = t.name;
                        nt.type = subst_type(t.type.get(), subst);
                        n->targets.push_back(std::move(nt));
                    }
                    n->pattern_type = d->pattern_type;
                    n->init = clone_expr_subst(d->init.get(), subst);
                    n->is_mutable = d->is_mutable;
                    return n;
                }
                case Statement::ExprStmt: {
                    auto *x = static_cast<const ExprStmt *>(s);
                    auto n = std::make_unique<ExprStmt>();
                    n->expr = clone_expr_subst(x->expr.get(), subst);
                    return n;
                }
                case Statement::Return: {
                    auto *r = static_cast<const ReturnStmt *>(s);
                    auto n = std::make_unique<ReturnStmt>();
                    n->value = clone_expr_subst(r->value.get(), subst);
                    return n;
                }
                case Statement::Throw: {
                    auto *t = static_cast<const ThrowStmt *>(s);
                    auto n = std::make_unique<ThrowStmt>();
                    n->value = clone_expr_subst(t->value.get(), subst);
                    return n;
                }
                case Statement::Try: {
                    auto *t = static_cast<const TryStmt *>(s);
                    auto n = std::make_unique<TryStmt>();
                    n->body = clone_stmt_subst(t->body.get(), subst);
                    n->error_name = t->error_name;
                    n->catch_body = clone_stmt_subst(t->catch_body.get(), subst);
                    n->finally_body = clone_stmt_subst(t->finally_body.get(), subst);
                    return n;
                }
                case Statement::If: {
                    auto *i = static_cast<const IfStmt *>(s);
                    auto n = std::make_unique<IfStmt>();
                    n->cond = clone_expr_subst(i->cond.get(), subst);
                    n->then_b = clone_stmt_subst(i->then_b.get(), subst);
                    n->else_b = clone_stmt_subst(i->else_b.get(), subst);
                    return n;
                }
                case Statement::While: {
                    auto *w = static_cast<const WhileStmt *>(s);
                    auto n = std::make_unique<WhileStmt>();
                    n->cond = clone_expr_subst(w->cond.get(), subst);
                    n->body = clone_stmt_subst(w->body.get(), subst);
                    return n;
                }
                case Statement::For: {
                    auto *f = static_cast<const ForStmt *>(s);
                    auto n = std::make_unique<ForStmt>();
                    n->variables = f->variables;
                    n->iterable = clone_expr_subst(f->iterable.get(), subst);
                    n->body = clone_stmt_subst(f->body.get(), subst);
                    return n;
                }
                case Statement::DoWhile: {
                    auto *d = static_cast<const DoWhileStmt *>(s);
                    auto n = std::make_unique<DoWhileStmt>();
                    n->body = clone_stmt_subst(d->body.get(), subst);
                    n->cond = clone_expr_subst(d->cond.get(), subst);
                    return n;
                }
                case Statement::Break: return std::make_unique<BreakStmt>();
                case Statement::Continue: return std::make_unique<ContinueStmt>();
                case Statement::Function: {
                    auto *f = static_cast<const FunctionDecl *>(s);
                    auto n = std::make_unique<FunctionDecl>();
                    n->annotations = f->annotations;
                    n->name = f->name;
                    for (auto &t: f->template_params) n->template_params.push_back(clone_type(t.get()));
                    for (auto &t: f->tparams) {
                        sinter::TemplateParam nt;
                        nt.name = t.name;
                        nt.constraint = subst_type(t.constraint.get(), subst);
                        n->tparams.push_back(std::move(nt));
                    }
                    for (auto &p: f->params) n->params.push_back(clone_param_subst(p, subst));
                    n->return_type = subst_type(f->return_type.get(), subst);
                    n->body = clone_stmt_subst(f->body.get(), subst);
                    n->is_variadic = f->is_variadic;
                    n->variadic_name = f->variadic_name;
                    n->variadic_elem_type = subst_type(f->variadic_elem_type.get(), subst);
                    n->is_operator = f->is_operator;
                    n->is_override = f->is_override;
                    n->is_pub = f->is_pub;
                    n->is_inline = f->is_inline;
                    n->is_static = f->is_static;
                    n->is_virtual = f->is_virtual;
                    n->is_abstract = f->is_abstract;
                    n->is_default = f->is_default;
                    n->aliases = f->aliases;
                    n->alias_name = f->alias_name;
                    n->ext_receiver = f->ext_receiver;
                    n->no_raii = f->no_raii;
                    return n;
                }
                case Statement::Class: {
                    auto *c = static_cast<const ClassDecl *>(s);
                    auto n = std::make_unique<ClassDecl>();
                    n->annotations = c->annotations;
                    n->name = c->name;
                    for (auto &t: c->tparams) {
                        sinter::TemplateParam nt;
                        nt.name = t.name;
                        nt.constraint = subst_type(t.constraint.get(), subst);
                        n->tparams.push_back(std::move(nt));
                    }
                    for (auto &p: c->ctor_params) n->ctor_params.push_back(clone_param_subst(p, subst));
                    n->bases = c->bases;
                    for (auto &args: c->base_args) {
                        std::vector<ExprPtr> na;
                        for (auto &a: args) na.push_back(clone_expr_subst(a.get(), subst));
                        n->base_args.push_back(std::move(na));
                    }
                    for (auto &m: c->members) n->members.push_back(clone_stmt_subst(m.get(), subst));
                    n->destructor = clone_stmt_subst(c->destructor.get(), subst);
                    if (c->copy_ctor) n->copy_ctor = as_fn(clone_stmt_subst(c->copy_ctor.get(), subst));
                    if (c->move_ctor) n->move_ctor = as_fn(clone_stmt_subst(c->move_ctor.get(), subst));
                    n->is_pub = c->is_pub;
                    n->is_abstract = c->is_abstract;
                    n->is_final = c->is_final;
                    n->is_struct = c->is_struct;
                    return n;
                }
                case Statement::Struct: {
                    auto *st = static_cast<const StructDecl *>(s);
                    auto n = std::make_unique<StructDecl>();
                    n->annotations = st->annotations;
                    n->name = st->name;
                    for (auto &f: st->fields) n->fields.push_back(clone_param_subst(f, subst));
                    for (auto &m: st->methods) n->methods.push_back(clone_stmt_subst(m.get(), subst));
                    n->base_class = st->base_class;
                    n->is_pub = st->is_pub;
                    return n;
                }
                case Statement::Interface: {
                    auto *i = static_cast<const InterfaceDecl *>(s);
                    auto n = std::make_unique<InterfaceDecl>();
                    n->annotations = i->annotations;
                    n->name = i->name;
                    for (auto &m: i->methods) n->methods.push_back(clone_stmt_subst(m.get(), subst));
                    n->bases = i->bases;
                    n->is_pub = i->is_pub;
                    return n;
                }
                case Statement::Enum: {
                    auto *e = static_cast<const EnumDecl *>(s);
                    auto n = std::make_unique<EnumDecl>();
                    n->annotations = e->annotations;
                    n->name = e->name;
                    n->variants = e->variants;
                    n->explicit_vals = e->explicit_vals;
                    n->is_pub = e->is_pub;
                    return n;
                }
                case Statement::Import: {
                    auto *i = static_cast<const ImportStmt *>(s);
                    auto n = std::make_unique<ImportStmt>();
                    n->path = i->path;
                    return n;
                }
                case Statement::Module: {
                    auto *m = static_cast<const ModuleStmt *>(s);
                    auto n = std::make_unique<ModuleStmt>();
                    n->annotations = m->annotations;
                    n->name = m->name;
                    n->is_block = m->is_block;
                    for (auto &x: m->body) n->body.push_back(clone_stmt_subst(x.get(), subst));
                    return n;
                }
                case Statement::Alias: {
                    auto *a = static_cast<const AliasStmt *>(s);
                    auto n = std::make_unique<AliasStmt>();
                    n->annotations = a->annotations;
                    n->name = a->name;
                    n->type = subst_type(a->type.get(), subst);
                    n->is_pub = a->is_pub;
                    return n;
                }
                case Statement::Comptime: {
                    auto *c = static_cast<const ComptimeStmt *>(s);
                    auto n = std::make_unique<ComptimeStmt>();
                    n->annotations = c->annotations;
                    n->body = clone_stmt_subst(c->body.get(), subst);
                    return n;
                }
                case Statement::BindCFunction: {
                    auto *b = static_cast<const BindCFunctionStmt *>(s);
                    auto n = std::make_unique<BindCFunctionStmt>();
                    n->annotations = b->annotations;
                    n->c_name = b->c_name;
                    for (auto &p: b->params) n->params.push_back(clone_param_subst(p, subst));
                    n->return_type = subst_type(b->return_type.get(), subst);
                    n->variadic = b->variadic;
                    return n;
                }
                case Statement::EmbedC: {
                    auto *e = static_cast<const EmbedCStmt *>(s);
                    auto n = std::make_unique<EmbedCStmt>();
                    n->annotations = e->annotations;
                    n->code = e->code;
                    n->in_header = e->in_header;
                    return n;
                }
                case Statement::Del: {
                    auto *d = static_cast<const DelStmt *>(s);
                    auto n = std::make_unique<DelStmt>();
                    n->annotations = d->annotations;
                    n->target = clone_expr_subst(d->target.get(), subst);
                    return n;
                }
                case Statement::PubBlock: {
                    auto *p = static_cast<const PubBlockStmt *>(s);
                    auto n = std::make_unique<PubBlockStmt>();
                    n->annotations = p->annotations;
                    for (auto &x: p->statements) n->statements.push_back(clone_stmt_subst(x.get(), subst));
                    return n;
                }
            }
            return nullptr;
        }

        // ==========================================================================
        // 使用处改写
        //
        // 就地修改 AST：把 `Box<i64>` 变成 `Box_i64`，把构造调用 `Box<i64>(...)`
        // 的 callee 改名，并把每条实例化需求推进 ctx.pending。实例化落地后再用
        // 同一个改写器处理克隆出来的模板体，于是嵌套泛型与模板内自引用都会自然
        // 产生新需求，主循环跑到不动点即可。
        // ==========================================================================

        /// 从字符串里取最后一段：`std.box.Box` → `Box`
        std::string last_segment(const std::string &s) {
            const auto p = s.rfind('.');
            return p == std::string::npos ? s : s.substr(p + 1);
        }

        // ==========================================================================
        // 模板查找：同名模板可在不同模块中共存（用户 `Box<T>` 与 std 的 `Box<T>`）
        // ==========================================================================
        // 模板键是 `模块下标#短名`；查找时优先本模块，其次任意模块（跨模块使用）。
        static std::string template_key(size_t mi, const std::string &short_name) {
            return std::to_string(mi) + "#" + short_name;
        }

        auto find_template = [](Context &ctx, const std::string &short_name, size_t mi) {
            auto it = ctx.templates.find(template_key(mi, short_name));
            if (it != ctx.templates.end()) return it;
            // 跨模块：短名匹配即可（实例仍然落在模板自己的声明模块）
            for (auto k = ctx.templates.begin(); k != ctx.templates.end(); ++k) {
                const auto hash = k->first.find('#');
                if (hash != std::string::npos && k->first.substr(hash + 1) == short_name) return k;
            }
            return ctx.templates.end();
        };

        /// 记录一条实例化需求（按实例名去重）
        void add_request(Context &ctx, const std::string &tmpl, const std::string &inst,
                         const TemplateInfo &info,
                         const std::vector<std::unique_ptr<TypeAnnotation> > &args,
                         size_t user_module) {
            // 跨模块使用：使用方需要 import 声明模板的模块
            if (user_module != info.module_index) {
                auto &paths = ctx.imports[user_module];
                const std::string &decl_mod = (*ctx.modules)[info.module_index].module_name;
                if (std::find(paths.begin(), paths.end(), decl_mod) == paths.end())
                    paths.push_back(decl_mod);
            }
            if (ctx.materialized.contains(inst)) return;
            for (auto &r: ctx.pending)
                if (r.inst_name == inst) return;
            Request req;
            req.tmpl_name = template_key(info.module_index, tmpl);
            req.inst_name = inst;
            for (auto &a: args) req.args.push_back(clone_type(a.get()));
            ctx.pending.push_back(std::move(req));
        }

        class Rewriter {
          public:
            Rewriter(Context &ctx, size_t module_index)
                : ctx_(ctx), module_index_(module_index) {}

            /// must 校验：实参个数 + `must number` 约束
            bool check_args(const ClassDecl *decl, size_t got, const std::string &name) {
                const size_t want = decl->tparams.size();
                if (got != want) {
                    ctx_.error = "generic type `" + name + "` requires " + std::to_string(want) +
                                 " type argument(s), but got " + std::to_string(got) + "";
                    return false;
                }
                for (size_t i = 0; i < want; ++i) {
                    auto &tp = decl->tparams[i];
                    if (!tp.constraint) continue;
                    // 目前只支持 `must number`：约束类型是数字类型名
                    if (tp.constraint->kind == TypeAnnotation::Named &&
                        tp.constraint->is_number_name()) {
                        const auto *arg = arg_at(i);
                        if (!arg || arg->kind != TypeAnnotation::Named || !arg->is_number_name()) {
                            ctx_.error = "generic type `" + name + "` type argument #" + std::to_string(i + 1) +
                                         " does not satisfy `must " + tp.constraint->name + "`";
                            return false;
                        }
                    }
                }
                return true;
            }

            bool stmt(StmtPtr &sp) {
                if (!sp) return true;
                switch (sp->kind) {
                    case Statement::Block: {
                        auto *b = static_cast<BlockStmt *>(sp.get());
                        for (auto &x: b->statements)
                            if (!stmt(x)) return false;
                        return true;
                    }
                    case Statement::VarDecl: {
                        auto *v = static_cast<VarDeclStmt *>(sp.get());
                        if (!type(v->type)) return false;
                        return expr(v->init);
                    }
                    case Statement::Destructure: {
                        auto *d = static_cast<DestructureStmt *>(sp.get());
                        for (auto &t: d->targets)
                            if (!type(t.type)) return false;
                        return expr(d->init);
                    }
                    case Statement::ExprStmt: {
                        auto *x = static_cast<ExprStmt *>(sp.get());
                        return expr(x->expr);
                    }
                    case Statement::Return: {
                        auto *r = static_cast<ReturnStmt *>(sp.get());
                        return expr(r->value);
                    }
                    case Statement::Throw: {
                        auto *t = static_cast<ThrowStmt *>(sp.get());
                        return expr(t->value);
                    }
                    case Statement::Try: {
                        auto *t = static_cast<TryStmt *>(sp.get());
                        return stmt(t->body) && stmt(t->catch_body) && stmt(t->finally_body);
                    }
                    case Statement::If: {
                        auto *i = static_cast<IfStmt *>(sp.get());
                        return expr(i->cond) && stmt(i->then_b) && stmt(i->else_b);
                    }
                    case Statement::While: {
                        auto *w = static_cast<WhileStmt *>(sp.get());
                        return expr(w->cond) && stmt(w->body);
                    }
                    case Statement::For: {
                        auto *f = static_cast<ForStmt *>(sp.get());
                        return expr(f->iterable) && stmt(f->body);
                    }
                    case Statement::DoWhile: {
                        auto *d = static_cast<DoWhileStmt *>(sp.get());
                        return stmt(d->body) && expr(d->cond);
                    }
                    case Statement::Function: {
                        auto *f = static_cast<FunctionDecl *>(sp.get());
                        return fn(*f);
                    }
                    case Statement::Class: {
                        auto *c = static_cast<ClassDecl *>(sp.get());
                        for (auto &p: c->ctor_params) {
                            if (!type(p.type)) return false;
                            if (!expr(p.default_value)) return false;
                        }
                        for (auto &args: c->base_args)
                            for (auto &a: args)
                                if (!expr(a)) return false;
                        for (auto &m: c->members)
                            if (!stmt(m)) return false;
                        if (c->destructor && !stmt(c->destructor)) return false;
                        if (c->copy_ctor && !fn(*c->copy_ctor)) return false;
                        if (c->move_ctor && !fn(*c->move_ctor)) return false;
                        return true;
                    }
                    case Statement::Struct: {
                        auto *st = static_cast<StructDecl *>(sp.get());
                        for (auto &f: st->fields) {
                            if (!type(f.type)) return false;
                            if (!expr(f.default_value)) return false;
                        }
                        for (auto &m: st->methods)
                            if (!stmt(m)) return false;
                        return true;
                    }
                    case Statement::Interface: {
                        auto *i = static_cast<InterfaceDecl *>(sp.get());
                        for (auto &m: i->methods)
                            if (!stmt(m)) return false;
                        return true;
                    }
                    case Statement::Module: {
                        auto *m = static_cast<ModuleStmt *>(sp.get());
                        for (auto &x: m->body)
                            if (!stmt(x)) return false;
                        return true;
                    }
                    case Statement::Alias: {
                        auto *a = static_cast<AliasStmt *>(sp.get());
                        return type(a->type);
                    }
                    case Statement::Comptime: {
                        auto *c = static_cast<ComptimeStmt *>(sp.get());
                        return stmt(c->body);
                    }
                    case Statement::BindCFunction: {
                        auto *b = static_cast<BindCFunctionStmt *>(sp.get());
                        for (auto &p: b->params)
                            if (!type(p.type)) return false;
                        return type(b->return_type);
                    }
                    case Statement::Del: {
                        auto *d = static_cast<DelStmt *>(sp.get());
                        return expr(d->target);
                    }
                    case Statement::PubBlock: {
                        auto *p = static_cast<PubBlockStmt *>(sp.get());
                        for (auto &x: p->statements)
                            if (!stmt(x)) return false;
                        return true;
                    }
                    default: return true;
                }
            }

            /// 改写一个类型注解：`Box<i64>` → `Box_i64`
            bool type(std::unique_ptr<TypeAnnotation> &tp) {
                if (!tp) return true;
                // 先处理内部：嵌套泛型 Box<Pair<i64, str>> 要先改写实参
                if (tp->inner_type && !type(tp->inner_type)) return false;
                for (auto &g: tp->generic_args)
                    if (!type(g)) return false;

                if (tp->kind != TypeAnnotation::Generic) return true;
                if (tp->generic_args.empty()) return true;

                const std::string key = last_segment(tp->name);
                auto it = find_template(ctx_, key, module_index_);
                if (it == ctx_.templates.end()) {
                    // 不是本项目里的泛型类型：交给后续阶段（内建泛型等）
                    return true;
                }
                if (!check_args(it->second.decl, tp->generic_args.size(), key)) return false;

                std::string inst = key;
                for (auto &a: tp->generic_args) inst += "_" + mangle_type(a.get());

                add_request(ctx_, key, inst, it->second, tp->generic_args, module_index_);

                // 就地退化成普通命名类型，保留引用修饰
                const bool cref = tp->is_const_ref, ref = tp->is_ref, rref = tp->is_rvalue_ref;
                tp->kind = TypeAnnotation::Named;
                tp->name = inst;
                tp->generic_args.clear();
                tp->inner_type.reset();
                tp->is_const_ref = cref;
                tp->is_ref = ref;
                tp->is_rvalue_ref = rref;
                return true;
            }

            bool expr(ExprPtr &ep) {
                if (!ep) return true;
                switch (ep->kind) {
                    case Expression::Call: {
                        auto *c = static_cast<CallExpr *>(ep.get());
                        if (!call(c)) return false;
                        if (!expr(c->callee)) return false;
                        for (auto &a: c->args)
                            if (!expr(a.value)) return false;
                        for (auto &a: c->arguments)
                            if (!expr(a)) return false;
                        for (auto &t: c->type_args)
                            if (!type(t)) return false;
                        return true;
                    }
                    case Expression::Binary: {
                        auto *b = static_cast<BinaryExpr *>(ep.get());
                        return expr(b->left) && expr(b->right);
                    }
                    case Expression::Unary: {
                        auto *u = static_cast<UnaryExpr *>(ep.get());
                        return expr(u->operand);
                    }
                    case Expression::MemberAccess: {
                        auto *m = static_cast<MemberAccessExpr *>(ep.get());
                        return expr(m->object);
                    }
                    case Expression::IndexAccess: {
                        auto *i = static_cast<IndexAccessExpr *>(ep.get());
                        return expr(i->object) && expr(i->index);
                    }
                    case Expression::Assignment: {
                        auto *a = static_cast<AssignmentExpr *>(ep.get());
                        return expr(a->target) && expr(a->value);
                    }
                    case Expression::Pipeline: {
                        auto *p = static_cast<PipelineExpr *>(ep.get());
                        return expr(p->left) && expr(p->right);
                    }
                    case Expression::Lambda: {
                        auto *l = static_cast<LambdaExpr *>(ep.get());
                        for (auto &p: l->params) {
                            if (!type(p.type)) return false;
                            if (!expr(p.default_value)) return false;
                        }
                        if (l->is_block) {
                            if (auto *b = std::get_if<StmtPtr>(&l->body)) return stmt(*b);
                        } else if (auto *ex = std::get_if<ExprPtr>(&l->body)) {
                            return expr(*ex);
                        }
                        return true;
                    }
                    case Expression::Ternary: {
                        auto *t = static_cast<TernaryExpr *>(ep.get());
                        return expr(t->cond) && expr(t->then_b) && expr(t->else_b);
                    }
                    case Expression::Range: {
                        auto *r = static_cast<RangeExpr *>(ep.get());
                        return expr(r->start) && expr(r->end);
                    }
                    case Expression::Cast: {
                        auto *c = static_cast<CastExpr *>(ep.get());
                        if (!expr(c->expr)) return false;
                        return type(c->target);
                    }
                    case Expression::Grouping: {
                        auto *g = static_cast<GroupingExpr *>(ep.get());
                        return expr(g->expr);
                    }
                    case Expression::IfExpr: {
                        auto *i = static_cast<IfExpr *>(ep.get());
                        return expr(i->cond) && expr(i->then_b) && expr(i->else_b);
                    }
                    case Expression::WhenExpr: {
                        auto *w = static_cast<WhenExpr *>(ep.get());
                        if (!expr(w->subject)) return false;
                        for (auto &br: w->branches) {
                            for (auto &pt: br.patterns) {
                                if (!expr(pt.value)) return false;
                                if (!expr(pt.lo)) return false;
                                if (!expr(pt.hi)) return false;
                            }
                            if (!expr(br.body)) return false;
                            if (!stmt(br.body_block)) return false;
                        }
                        if (!expr(w->else_branch)) return false;
                        return stmt(w->else_block);
                    }
                    case Expression::ArrayLiteral: {
                        auto *a = static_cast<ArrayLiteralExpr *>(ep.get());
                        for (auto &x: a->elements)
                            if (!expr(x)) return false;
                        return true;
                    }
                    case Expression::StringTemplate: {
                        auto *s = static_cast<StringTemplateExpr *>(ep.get());
                        for (auto &pt: s->parts)
                            if (pt.expr && !expr(pt.expr)) return false;
                        return true;
                    }
                    case Expression::ScopeCall: {
                        auto *s = static_cast<ScopeCallExpr *>(ep.get());
                        return expr(s->receiver) && expr(s->body_expr) && stmt(s->body);
                    }
                    case Expression::BlockExprKind: {
                        auto *b = static_cast<BlockExpr *>(ep.get());
                        return expr(b->result) && stmt(b->block);
                    }
                    default: return true;
                }
            }

            bool fn(FunctionDecl &f) {
                for (auto &p: f.params) {
                    if (!type(p.type)) return false;
                    if (!expr(p.default_value)) return false;
                }
                if (!type(f.return_type)) return false;
                if (!type(f.variadic_elem_type)) return false;
                return stmt(f.body);
            }

          private:
            /// must 校验时需要看当前这次调用的实参：临时记下来
            const TypeAnnotation *arg_at(size_t i) const {
                if (!cur_args_ || i >= cur_args_->size()) return nullptr;
                return (*cur_args_)[i].get();
            }

            /// 构造调用：`Box<i64>(...)` 的 callee 改写为实例名
            bool call(CallExpr *c) {
                auto *id = dynamic_cast<IdentifierExpr *>(c->callee.get());
                if (!id) return true;   // mod.Box<i64>(...) 走 callee 里的 MemberAccess 分支

                const std::string key = last_segment(id->name);
                auto it = find_template(ctx_, key, module_index_);
                if (it == ctx_.templates.end()) return true;

                // 只处理形如 `Box<i64>(...)` 的构造调用。
                // 缺类型实参时：
                //   * 本模块自己声明的模板 → 必须报错（漏写 `<T>` 是错误）
                //   * 其他模块的同名模板 → 可能是用户自己的同名非泛型类型，放行
                if (c->type_args.empty()) {
                    if (it->second.module_index == module_index_) {
                        ctx_.error = "generic type `" + key + "` requires explicit type arguments, e.g. `" + key + "<i64>`";
                        return false;
                    }
                    return true;
                }
                cur_args_ = &c->type_args;
                const bool ok = check_args(it->second.decl, c->type_args.size(), key);
                if (!ok) { cur_args_ = nullptr; return false; }
                cur_args_ = nullptr;

                std::string inst = key;
                for (auto &a: c->type_args) inst += "_" + mangle_type(a.get());
                add_request(ctx_, key, inst, it->second, c->type_args, module_index_);
                id->name = inst;
                c->type_args.clear();
                return true;
            }

            Context &ctx_;
            size_t module_index_;
            const std::vector<std::unique_ptr<TypeAnnotation> > *cur_args_ = nullptr;
        };

        // ==========================================================================
        // 模板收集：把每个模块里的 `class X<T>` 登记成模板
        // ==========================================================================
        bool collect_templates(Context &ctx) {
            for (size_t mi = 0; mi < ctx.modules->size(); ++mi) {
                auto &mod = *((*ctx.modules)[mi].program);
                std::function<bool(std::vector<StmtPtr> &)> scan = [&](std::vector<StmtPtr> &stmts) {
                    for (auto &s: stmts) {
                        if (auto *ms = dynamic_cast<ModuleStmt *>(s.get())) {
                            if (!scan(ms->body)) return false;
                            continue;
                        }
                        if (auto *pb = dynamic_cast<PubBlockStmt *>(s.get())) {
                            if (!scan(pb->statements)) return false;
                            continue;
                        }
                        auto *c = dynamic_cast<ClassDecl *>(s.get());
                        if (!c || c->tparams.empty()) continue;
                        // 同名模板允许存在于不同模块（用户自己的 `Box<T>` 与
                        // std 提供的 `Box<T>` 各管各的），只有模块内重名才是错误。
                        const std::string key = template_key(mi, c->name);
                        if (ctx.templates.contains(key)) {
                            ctx.error = "generic type `" + c->name + "` is defined more than once";
                            return false;
                        }
                        TemplateInfo info;
                        info.decl = c;
                        info.module_index = mi;
                        ctx.templates[key] = info;
                    }
                    return true;
                };
                if (!scan(mod.statements)) return false;
            }
            return true;
        }

        // ==========================================================================
        // 实例化落地：把模板深拷贝一份，代入类型参数、改名字，插进声明模块
        // ==========================================================================
        bool instantiate_one(Context &ctx, const Request &req) {
            const auto tit = ctx.templates.find(req.tmpl_name);   // 键已含声明模块
            if (tit == ctx.templates.end()) {
                ctx.error = "unknown generic type `" + req.tmpl_name + "`";
                return false;
            }
            const auto &info = tit->second;

            // A generic instance that uses a user-defined class must be generated
            // in the module that declares that class: only that module knows the
            // class's C struct name (`module_plain`). Instances whose arguments are
            // all built-ins stay in the template's module.
            static const std::set<std::string> kBuiltinArgs = {
                "i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64", "u128",
                "number", "f32", "f64", "bool", "str", "string", "cstr", "ptr", "char",
                "void",
            };
            size_t target_index = info.module_index;
            bool moved_for_user_type = false;
            for (auto &a: req.args) {
                if (!a || a->kind != TypeAnnotation::Named) continue;
                if (kBuiltinArgs.contains(a->name)) continue;
                for (size_t mi = 0; mi < ctx.modules->size(); ++mi) {
                    auto &mod = *((*ctx.modules)[mi].program);
                    bool found = false;
                    for (auto &st: mod.statements) {
                        if (auto *c = dynamic_cast<ClassDecl *>(st.get()); c && c->name == a->name) {
                            target_index = mi;
                            found = true;
                            break;
                        }
                    }
                    if (found) { moved_for_user_type = true; break; }
                }
                if (moved_for_user_type) break;
            }
            auto &decl_mod = *((*ctx.modules)[target_index].program);
            if (moved_for_user_type) {
                // The cloned methods may call helpers from the template's module.
                ctx.imports[target_index].push_back((*ctx.modules)[info.module_index].module_name);
            }

            // 冲突检查：目标模块里已有同名类（用户手写的 Box_i64）
            for (auto &s: decl_mod.statements) {
                if (auto *c = dynamic_cast<ClassDecl *>(s.get());
                    c && c->name == req.inst_name) {
                    ctx.error = "generic instance `" + req.inst_name +
                                "` conflicts with an existing type of the same name; rename one of them";
                    return false;
                }
            }

            // 类型参数替换表
            Subst subst;
            for (size_t i = 0; i < info.decl->tparams.size() && i < req.args.size(); ++i)
                subst[info.decl->tparams[i].name] = clone_type(req.args[i].get());

            // 深拷贝模板体（此时内部仍是 Box<i64> 形式，紧接着用 Rewriter 统一改写）
            auto cloned = clone_stmt_subst(info.decl, subst);
            auto *inst = dynamic_cast<ClassDecl *>(cloned.release());
            if (!inst) {
                ctx.error = "internal error: generic template is not a class declaration";
                return false;
            }
            inst->name = req.inst_name;
            inst->tparams.clear();
            // 实例保留模板形参的名字表：代码生成阶段要靠它把实例名尾部的
            // `_str` / `_str_i64` 还原成类型实参，从而把按 T 分派的多态调用
            // （`sinter_gen_clone(v)` 之类）归约成具体函数名。
            if (ctx.table) ctx.table->add(req.inst_name, info.decl->tparams, ctx.modules->at(target_index).module_name);

            // 记录已落地，避免自引用时无限递归
            ctx.materialized[req.inst_name] = target_index;

            // 改写实例体内剩余的泛型使用（嵌套泛型 / 自引用），
            // 期间产生的新需求会追加到 ctx.pending
            StmtPtr holder(inst);
            Rewriter rw(ctx, info.module_index);
            if (!rw.stmt(holder)) return false;

            decl_mod.statements.push_back(std::move(holder));
            return true;
        }

        // ==========================================================================
        // 给使用方模块补 import（跨模块使用泛型类时需要看到实例的声明）
        // ==========================================================================
        void apply_imports(Context &ctx) {
            for (auto &[mi, paths]: ctx.imports) {
                if (mi >= ctx.modules->size()) continue;
                auto &mod = *((*ctx.modules)[mi].program);
                for (auto &path: paths) {
                    bool exists = false;
                    for (auto &s: mod.statements) {
                        if (auto *imp = dynamic_cast<ImportStmt *>(s.get());
                            imp && imp->path == path) {
                            exists = true;
                            break;
                        }
                    }
                    if (exists) continue;
                    auto imp = std::make_unique<ImportStmt>();
                    imp->path = path;
                    mod.statements.insert(mod.statements.begin(), std::move(imp));
                }
            }
        }

        // ==========================================================================
        // 把模板本体从各模块 AST 里移除（单态化之后不再需要）
        // ==========================================================================
        void remove_templates(Context &ctx) {
            for (auto &gm: *ctx.modules) {
                auto &mod = *gm.program;
                std::function<void(std::vector<StmtPtr> &)> strip = [&](std::vector<StmtPtr> &stmts) {
                    std::erase_if(stmts, [](const StmtPtr &s) {
                        if (auto *c = dynamic_cast<const ClassDecl *>(s.get()))
                            return !c->tparams.empty();
                        return false;
                    });
                    for (auto &s: stmts) {
                        if (auto *ms = dynamic_cast<ModuleStmt *>(s.get())) strip(ms->body);
                        else if (auto *pb = dynamic_cast<PubBlockStmt *>(s.get())) strip(pb->statements);
                    }
                };
                strip(mod.statements);
            }
        }
    } // namespace

    bool instantiate_generic_types(std::vector<GenericModule> &modules, std::string &error,
                                   GenericTable *table) {
        bool any_template = false;
        for (auto &m: modules) {
            if (!m.program) continue;
            for (auto &s: m.program->statements) {
                auto *c = dynamic_cast<ClassDecl *>(s.get());
                if (c && !c->tparams.empty()) { any_template = true; break; }
                if (auto *ms = dynamic_cast<ModuleStmt *>(s.get())) {
                    for (auto &x: ms->body) {
                        auto *mc = dynamic_cast<ClassDecl *>(x.get());
                        if (mc && !mc->tparams.empty()) { any_template = true; break; }
                    }
                }
                if (any_template) break;
            }
            if (any_template) break;
        }

        Context ctx;
        ctx.modules = &modules;
        ctx.table = table;

        // 1) 收集模板
        if (!collect_templates(ctx)) {
            error = ctx.error;
            return false;
        }

        // 1.5) 泛型基类：`class IntArray : Array<i64>` 的基类写在 `bases` 里，
        // 而 Rewriter 只改写「类型标注」，不会碰这个名字列表。这里按
        // （模板短名，实参短名列表）手工登记一条需求，让 `Array<i64>` 真的被
        // 单态化成 `Array_i64`，继承链才有东西可内嵌。
        for (size_t mi = 0; mi < modules.size(); ++mi) {
            if (!modules[mi].program) continue;
            for (auto &s: modules[mi].program->statements) {
                auto *c = dynamic_cast<ClassDecl *>(s.get());
                if (!c || !c->tparams.empty() || c->bases.empty()) continue;
                const size_t sep = c->bases[0].find('\x01');   // 语义层用 \x01 存实参
                if (sep == std::string::npos) continue;
                const std::string tmpl = c->bases[0].substr(0, sep);
                std::vector<std::unique_ptr<TypeAnnotation> > args;
                std::string rest = c->bases[0].substr(sep + 1);
                // 实参用 `,` 分隔（已由语义层归一过，不含嵌套泛型）
                while (!rest.empty()) {
                    const auto comma = rest.find(',');
                    const std::string one = rest.substr(0, comma);
                    auto ta = std::make_unique<TypeAnnotation>();
                    ta->kind = TypeAnnotation::Named;
                    ta->name = one;
                    args.push_back(std::move(ta));
                    if (comma == std::string::npos) break;
                    rest = rest.substr(comma + 1);
                }
                c->bases[0] = tmpl;   // 名字列表里只留模板短名
                auto it = find_template(ctx, tmpl, mi);
                if (it == ctx.templates.end()) continue;
                // 实参个数必须与模板形参对齐（`must number` 约束在这里不做检查：
                // 基类写的是具体类型，约束在泛型声明处已经被用户遵守）。
                if (args.size() != it->second.decl->tparams.size()) {
                    error = "generic base `" + tmpl + "` of `" + c->name + "` requires " +
                            std::to_string(it->second.decl->tparams.size()) +
                            " type argument(s), but got " + std::to_string(args.size());
                    return false;
                }
                std::string inst = tmpl;
                for (auto &a: args) inst += "_" + mangle_type(a.get());
                add_request(ctx, tmpl, inst, it->second, args, mi);
            }
        }

        // 2) 逐模块改写使用处（同时登记实例化需求）
        //
        // 注意：模板声明本体必须跳过。模板体里的 `Node<T>` 只有在代入具体类型
        // 之后才有意义；若在这里就地改写，`T` 还只是类型参数名，会算出错误
        // 的实例名（如 Node_T），并且污染模板本体。模板体统一在
        // instantiate_one 里、对克隆副本做改写。
        for (size_t mi = 0; mi < modules.size(); ++mi) {
            if (!modules[mi].program) continue;
            Rewriter rw(ctx, mi);
            auto &stmts = modules[mi].program->statements;
            for (size_t i = 0; i < stmts.size(); ++i) {
                if (auto *c = dynamic_cast<ClassDecl *>(stmts[i].get());
                    c && !c->tparams.empty())
                    continue;
                if (!rw.stmt(stmts[i])) {
                    error = ctx.error.empty() ? "failed to rewrite generic type" : ctx.error;
                    return false;
                }
            }
        }

        // 3) 跑工作列表直到不动点（支持模板内自引用与传递实例化）
        while (!ctx.pending.empty()) {
            Request req = std::move(ctx.pending.back());
            ctx.pending.pop_back();
            if (ctx.materialized.contains(req.inst_name)) continue;
            if (!instantiate_one(ctx, req)) {
                error = ctx.error.empty() ? "failed to instantiate generic type" : ctx.error;
                return false;
            }
        }

        // 4) 模板本体移出 AST
        remove_templates(ctx);

        // 5) 跨模块使用方补 import
        apply_imports(ctx);

        if (!any_template) return true;
        return true;
    }
} // namespace sinter