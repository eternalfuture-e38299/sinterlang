// sinter: conservative ownership/lifetime checks (see ownership.cpp).
#pragma once

#include "front/ast.hpp"
#include "sem/semantic.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace sinter {
    class OwnershipChecker {
    public:
        explicit OwnershipChecker(Semantic &sem) : sem_(sem) {}
        void check_program(Program &p);

    private:
        struct VarState {
            bool deleted = false;
            bool address_taken = false;
            std::string points_to;
        };

        Semantic &sem_;
        std::vector<std::unordered_map<std::string, VarState> > scopes_;
        std::unordered_map<std::string, std::string> pointer_aliases_;

        void check_decl(Statement *s);
        void check_function(FunctionDecl *f);
        void enter_scope();
        void leave_scope();
        VarState *find(const std::string &name);
        void stmt(Statement *s);
        void expr(Expression *e);
        void check_return(const ReturnStmt *r);
        void check_del(const DelStmt *d);
        std::string root_local(const Expression *e) const;

        void error(const std::string &code, const std::string &msg,
                   const std::string &hint, const Statement *s);
        void error_expr(const std::string &code, const std::string &msg,
                        const std::string &hint, const Expression *e);
    };
}
