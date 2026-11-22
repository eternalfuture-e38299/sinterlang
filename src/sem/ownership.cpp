// sinter ownership checks: a conservative pass for obvious local-lifetime errors.
//
// This is not a Rust-style borrow checker. Sinter still has raw `ptr`/`*T`
// values and arbitrary C interop, so the best a static pass can do is reject
// patterns that are *certain* to dangle inside one function:
//
//   - returning `&local` (or a pointer that aliases it)
//   - `del x` twice, or using `x` after `del x`
//   - `del x` while `&x` was taken and is still used later
//   - storing `&local` into an object field / static (escaping the scope)
//
// Anything involving raw integers, C functions or cross-module aliasing is
// left to the programmer; those cases are documented as unsafe.
#include "sem/ownership.hpp"

namespace sinter {
    void OwnershipChecker::check_program(Program &p) {
        for (auto &s: p.statements) check_decl(s.get());
    }

    void OwnershipChecker::check_decl(Statement *s) {
        if (!s) return;
        if (auto *f = dynamic_cast<FunctionDecl *>(s)) {
            check_function(f);
            return;
        }
        if (auto *c = dynamic_cast<ClassDecl *>(s)) {
            for (auto &m: c->members) check_decl(m.get());
            return;
        }
        if (auto *pb = dynamic_cast<PubBlockStmt *>(s)) {
            for (auto &x: pb->statements) check_decl(x.get());
            return;
        }
        if (auto *ms = dynamic_cast<ModuleStmt *>(s)) {
            if (ms->is_block)
                for (auto &x: ms->body) check_decl(x.get());
        }
    }

    void OwnershipChecker::check_function(FunctionDecl *f) {
        if (!f || !f->body) return;
        scopes_.clear();
        pointer_aliases_.clear();
        enter_scope();
        for (auto &prm: f->params) {
            if (!prm.name.empty()) scopes_.back()[prm.name] = VarState{};
        }
        stmt(f->body.get());
        leave_scope();
    }

    void OwnershipChecker::enter_scope() { scopes_.emplace_back(); }

    void OwnershipChecker::leave_scope() {
        if (!scopes_.empty()) scopes_.pop_back();
    }

    OwnershipChecker::VarState *OwnershipChecker::find(const std::string &name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto hit = it->find(name);
            if (hit != it->end()) return &hit->second;
        }
        return nullptr;
    }

    void OwnershipChecker::error(const std::string &code, const std::string &msg,
                                 const std::string &hint, const Statement *s) {
        sem_.report_error_ex(code, msg, hint, s->line, s->col);
    }

    void OwnershipChecker::error_expr(const std::string &code, const std::string &msg,
                                      const std::string &hint, const Expression *e) {
        sem_.report_error_ex(code, msg, hint, e->line, e->col);
    }

    std::string OwnershipChecker::root_local(const Expression *e) const {
        if (!e) return {};
        if (auto *g = dynamic_cast<const GroupingExpr *>(e)) return root_local(g->expr.get());
        if (auto *id = dynamic_cast<const IdentifierExpr *>(e)) {
            auto hit = pointer_aliases_.find(id->name);
            return hit == pointer_aliases_.end() ? id->name : hit->second;
        }
        if (auto *u = dynamic_cast<const UnaryExpr *>(e); u && u->op == TokenType::Ampersand) {
            if (auto *id = dynamic_cast<const IdentifierExpr *>(u->operand.get())) return id->name;
        }
        return {};
    }

    void OwnershipChecker::check_return(const ReturnStmt *r) {
        if (!r || !r->value) return;
        if (auto *u = dynamic_cast<UnaryExpr *>(r->value.get());
            u && u->op == TokenType::Ampersand) {
            if (auto *id = dynamic_cast<IdentifierExpr *>(u->operand.get())) {
                if (find(id->name)) {
                    error_expr("E0901", "returning the address of local variable '" + id->name + "'",
                               "return a heap object, or pass an output parameter instead", u);
                }
            }
        }
        if (auto *id = dynamic_cast<IdentifierExpr *>(r->value.get())) {
            auto hit = pointer_aliases_.find(id->name);
            if (hit != pointer_aliases_.end()) {
                error_expr("E0901", "returning pointer '" + id->name + "', which aliases local '" +
                                        hit->second + "'",
                           "allocate the value on the heap before returning it", id);
                return;
            }
        }
        expr(r->value.get());
    }

    void OwnershipChecker::check_del(const DelStmt *d) {
        auto *id = dynamic_cast<IdentifierExpr *>(d->target.get());
        if (!id) { expr(d->target.get()); return; }
        VarState *v = find(id->name);
        if (!v) { expr(d->target.get()); return; }
        if (v->deleted) {
            error_expr("E0903", "variable '" + id->name + "' is deleted more than once",
                       "remove the extra `del`, or set it to null first", id);
        }
        if (v->address_taken) {
            error_expr("E0904", "cannot delete '" + id->name + "' while `&" + id->name +
                                    "` may still be used",
                       "delete only after the last use of the pointer", id);
        }
        v->deleted = true;
    }

    void OwnershipChecker::stmt(Statement *s) {
        if (!s) return;
        if (auto *b = dynamic_cast<BlockStmt *>(s)) {
            enter_scope();
            for (auto &x: b->statements) stmt(x.get());
            leave_scope();
            return;
        }
        if (auto *v = dynamic_cast<VarDeclStmt *>(s)) {
            if (v->init) {
                expr(v->init.get());
                if (auto *u = dynamic_cast<UnaryExpr *>(v->init.get());
                    u && u->op == TokenType::Ampersand) {
                    if (auto *id = dynamic_cast<IdentifierExpr *>(u->operand.get())) {
                        if (VarState *target = find(id->name)) target->address_taken = true;
                        pointer_aliases_[v->name] = id->name;
                    }
                }
            }
            scopes_.back()[v->name] = VarState{};
            return;
        }
        if (auto *d = dynamic_cast<DelStmt *>(s)) { check_del(d); return; }
        if (auto *r = dynamic_cast<ReturnStmt *>(s)) { check_return(r); return; }
        if (auto *e = dynamic_cast<ExprStmt *>(s)) {
            if (auto *a = dynamic_cast<AssignmentExpr *>(e->expr.get())) {
                expr(a->value.get());
                if (dynamic_cast<MemberAccessExpr *>(a->target.get())) {
                    std::string local;
                    if (auto *u = dynamic_cast<UnaryExpr *>(a->value.get());
                        u && u->op == TokenType::Ampersand) {
                        if (auto *id = dynamic_cast<IdentifierExpr *>(u->operand.get()))
                            if (find(id->name)) local = id->name;
                    } else if (auto *id = dynamic_cast<IdentifierExpr *>(a->value.get())) {
                        auto hit = pointer_aliases_.find(id->name);
                        if (hit != pointer_aliases_.end()) local = hit->second;
                    }
                    if (!local.empty()) {
                        error_expr("E0905", "storing a pointer to local '" + local +
                                                "' in a field or global",
                                   "the local will die before the field/global is read", a);
                    }
                }
                expr(a->target.get());
                return;
            }
            expr(e->expr.get());
            return;
        }
        if (auto *i = dynamic_cast<IfStmt *>(s)) {
            expr(i->cond.get());
            stmt(i->then_b.get());
            stmt(i->else_b.get());
            return;
        }
        if (auto *w = dynamic_cast<WhileStmt *>(s)) {
            expr(w->cond.get());
            stmt(w->body.get());
            return;
        }
        if (auto *f = dynamic_cast<ForStmt *>(s)) {
            expr(f->iterable.get());
            enter_scope();
            for (auto &name: f->variables) scopes_.back()[name] = VarState{};
            stmt(f->body.get());
            leave_scope();
            return;
        }
        if (auto *d = dynamic_cast<DoWhileStmt *>(s)) {
            stmt(d->body.get());
            expr(d->cond.get());
            return;
        }
        if (auto *t = dynamic_cast<TryStmt *>(s)) {
            stmt(t->body.get());
            enter_scope();
            if (!t->error_name.empty()) scopes_.back()[t->error_name] = VarState{};
            stmt(t->catch_body.get());
            leave_scope();
            stmt(t->finally_body.get());
            return;
        }
        if (auto *th = dynamic_cast<ThrowStmt *>(s)) {
            expr(th->value.get());
            return;
        }
    }

    void OwnershipChecker::expr(Expression *e) {
        if (!e) return;
        if (auto *id = dynamic_cast<IdentifierExpr *>(e)) {
            if (VarState *v = find(id->name); v && v->deleted) {
                error_expr("E0902", "variable '" + id->name + "' is used after `del`",
                           "remove the use, or do not delete it before this point", id);
                v->deleted = false;   // report once
            }
            return;
        }
        if (auto *u = dynamic_cast<UnaryExpr *>(e)) {
            if (u->op == TokenType::Ampersand) {
                if (auto *id = dynamic_cast<IdentifierExpr *>(u->operand.get())) {
                    if (VarState *v = find(id->name)) v->address_taken = true;
                    return;
                }
            }
            expr(u->operand.get());
            return;
        }
        if (auto *a = dynamic_cast<AssignmentExpr *>(e)) {
            expr(a->value.get());
            expr(a->target.get());
            return;
        }
        if (auto *c = dynamic_cast<CallExpr *>(e)) {
            expr(c->callee.get());
            for (auto &arg: c->args) expr(arg.value.get());
            return;
        }
        if (auto *b = dynamic_cast<BinaryExpr *>(e)) {
            expr(b->left.get());
            expr(b->right.get());
            return;
        }
        if (auto *m = dynamic_cast<MemberAccessExpr *>(e)) {
            expr(m->object.get());
            return;
        }
        if (auto *g = dynamic_cast<GroupingExpr *>(e)) { expr(g->expr.get()); return; }
        if (auto *ix = dynamic_cast<IndexAccessExpr *>(e)) {
            expr(ix->object.get());
            expr(ix->index.get());
            return;
        }
        if (auto *t = dynamic_cast<TernaryExpr *>(e)) {
            expr(t->cond.get());
            expr(t->then_b.get());
            expr(t->else_b.get());
            return;
        }
        if (auto *cast = dynamic_cast<CastExpr *>(e)) { expr(cast->expr.get()); return; }
        if (auto *r = dynamic_cast<RangeExpr *>(e)) {
            expr(r->start.get());
            expr(r->end.get());
            return;
        }
        if (auto *al = dynamic_cast<ArrayLiteralExpr *>(e)) {
            for (auto &x: al->elements) expr(x.get());
            return;
        }
        if (auto *lam = dynamic_cast<LambdaExpr *>(e)) {
            if (auto *body = std::get_if<ExprPtr>(&lam->body)) expr(body->get());
            else if (auto *body = std::get_if<StmtPtr>(&lam->body)) stmt(body->get());
            return;
        }
        if (auto *sc = dynamic_cast<ScopeCallExpr *>(e)) {
            expr(sc->receiver.get());
            return;
        }
        if (auto *w = dynamic_cast<WhenExpr *>(e)) {
            expr(w->subject.get());
            for (auto &br: w->branches) {
                for (auto &pat: br.patterns) { expr(pat.value.get()); }
                if (br.body) expr(br.body.get());
                if (br.body_block) stmt(br.body_block.get());
            }
            expr(w->else_branch.get());
            if (w->else_block) stmt(w->else_block.get());
            return;
        }
    }
}
