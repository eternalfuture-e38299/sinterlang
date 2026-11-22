// sinter codegen header
#pragma once
#include "front/ast.hpp"
#include "front/generic_mono.hpp"
#include "sem/semantic.hpp"
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

namespace sinter {
    class Codegen {
    public:
        explicit Codegen(Semantic &s) : sem_(s) {}
        /// 泛型实例表（单态化副产物）：实例名 → 类型实参短名。
        /// 有了它，实例方法体里 `sinter_gen_clone(v)` 这类按 T 分派的调用
        /// 才能落到 `sinter_gen_clone_str(v)`。生命周期由调用方保证。
        void set_generic_table(const GenericTable *t) { generic_table_ = t; }

        using GenericFunctionInstantiations =
            std::unordered_map<FunctionDecl *,
                               std::vector<std::unordered_map<std::string, std::string> > >;
        /// Instantiations requested by every module of the program, grouped by
        /// the module that defines the generic function. Filled by the driver.
        void set_generic_function_instantiations(const GenericFunctionInstantiations *m) {
            external_generic_insts_ = m;
        }
        void collect_generic_instantiations(const Program &p, GenericFunctionInstantiations &out);

        std::string generate_c(Program &p, const std::string &module_name);
        std::string generate_h(Program &p, const std::string &module_name);

        /// 运行时支持库：独立生成 sinter_runtime.h / sinter_runtime.c，
        /// 不再逐模块内嵌（避免每个模块头文件重复一份实现）。
        static std::string runtime_header();
        static std::string runtime_source();

    private:
        Semantic &sem_;
        const GenericTable *generic_table_ = nullptr;
        const GenericFunctionInstantiations *external_generic_insts_ = nullptr;
        std::ostringstream *generic_proto_out_ = nullptr;
        std::ostringstream h_pre_, h_funs_, h_types_;
        std::ostringstream c_embed_, c_funs_, pending_;
        int indent_ = 0, tmp_ = 0;
        std::unordered_map<std::string, std::string> locals_;   // sinter 名 -> C 表达式
        /// 模块级 `val` / `var`（含 `comptime` 块展开出来的常量）。
        /// `expr_ident` 找不到局部变量时要落到这里，否则会生成裸名字
        /// （`println(LIMIT)` → `LIMIT`，而真实符号是 `ct_g_LIMIT`）。
        std::unordered_set<std::string> module_globals_;
        std::unordered_map<std::string, std::string> local_types_; // sinter 名 -> C 类型
        /// `T&` 局部/形参：自动解引用（类引用除外，类值本身就是指针）。
        std::unordered_set<std::string> ref_vars_;
        std::unordered_set<std::string> ref_class_vars_;
        std::unordered_set<std::string> used_;
        /// 当前正在生成的函数/构造的形参 C 名：fresh() 查重时一并避开，
        /// 否则最外层块里的同名局部变量会与形参重名（C 不允许同作用域重声明）。
        std::unordered_set<std::string> cur_params_;
        std::string current_class_;
        /// 正在生成的方法名（含 `operator...` 的原名），用于判断自递归调用。
        std::string current_method_;
        std::string current_class_c_;
        std::string module_name_;
        std::string module_ident_;

        // 声明收集：pub 的进头文件（导出），非 pub 的只进本模块 .c（模块内私有）
        struct DeclChunk {
            bool is_pub = false;
            std::string text;
        };
        std::vector<DeclChunk> type_decls_;
        std::vector<DeclChunk> fun_decls_;
        // vtable 定义：必须排在类型与函数原型之后、函数定义之前
        std::vector<std::string> vtable_defs_;

        struct ScopeVar {
            std::string name;
            std::string c_name;
            std::string type_c;
            bool raii;
            // 资源类别：类实例走 `<T>_del`、字符串走 sinter_string_free、
            // 内建数组走 free。空串表示按类实例处理（向后兼容）。
            std::string kind;
            /// kind == "array" 且元素类型带析构时：元素析构函数名与元素个数。
            /// 作用域退出时先逐个 `X_del(arr[i])`，再 free 缓冲。
            std::string elem_del;
            int64_t elem_count = 0;
        };
        std::vector<std::vector<ScopeVar> > scopes_;
        /// Number of cleanup-stack entries pushed by each scope (exception unwinding).
        std::vector<int> scope_cleanup_counts_;
        /// >0 while generating code that may be unwound by `throw`.
        int try_depth_ = 0;
        /// Try blocks whose `finally` must run before an early return/break/continue.
        struct ActiveFinally { const TryStmt *t; size_t loop_depth; bool in_catch; };
        std::vector<ActiveFinally> active_finally_;
        // 当前函数体的作用域深度（enter_scope 之后记下），return 时清理到这一层
        size_t fn_scope_depth_ = 0;
        // 当前函数是否带 @no_raii：提前退出路径也不得析构
        bool fn_no_raii_ = false;
        // 循环栈：每层记录循环体的作用域深度，break/continue 清理到该层（不含）
        std::vector<size_t> loop_scope_depths_;
        // 已被提前退出路径（return/break/continue）清理过的作用域下标，
        // leave_scope 据此避免重复生成析构。
        std::unordered_set<size_t> exited_scopes_;
        // true = 正在产出 .c（false = 产出 .h）。
        // generate_c 与 generate_h 会各遍历一次程序，诊断只应在 .c 那一轮报告。
        bool emit_source_only_ = true;

        void emit(const std::string &s) { c_funs_ << std::string(indent_ * 4, ' ') << s << "\n"; }
        std::string fresh(const std::string &b);
        /// 形参在生成 C 里的名字：语义层已算好（snake_case），缺失时按同一规则兜底
        static std::string param_c_name(const ParamInfo &p) {
            return p.c_name.empty() ? Semantic::to_snake_ident(p.name) : p.c_name;
        }
        // ---- 泛型单态化：类型实参 → 名字片段（定义、原型与调用点必须完全一致） ----
        /// 把 C 类型（`int64_t` / `sinter_string` / `struct foo*`）或 Sinter 短名
        /// 归一成 `_i64` / `_str` / `_foo_ptr` 里的片段（不含前导下划线）。
        static std::string generic_type_token(const std::string &t);
        /// 调用点的泛型实参替换表：显式 type_args 优先，其次按形参位置从实参推断。
        /// collect_generic_instantiations 与调用点共用它，保证两侧算出同一个后缀。
        std::unordered_map<std::string, std::string>
        generic_subst_for_call(const FunctionDecl *decl, CallExpr *c,
                               const std::function<std::string(Expression *)> &arg_type = {});
        /// 由替换表拼出 `_i64_str` 形式的后缀
        std::string generic_suffix(const std::vector<std::string> &tparam_names,
                                   const std::unordered_map<std::string, std::string> &subst);
        /// 同上，但支持 `Args...` 类型参数包：包内每个类型各贡献一段后缀。
        /// 表格里 `Args#<i>` 存第 i 个实参类型，`Args#count` 存个数。
        std::string generic_pack_suffix(const std::vector<TemplateParam> &tparams,
                                        const std::unordered_map<std::string, std::string> &subst);
        /// 泛型函数/方法单态化后的 C 名（含模块前缀与类前缀）
        std::string generic_fn_c_name(const FunctionInfo &fi,
                                      const std::unordered_map<std::string, std::string> &subst);
        /// 找到声明对应的语义条目（单态化时判断它是不是方法、owner 是谁）
        const FunctionInfo *fi_of_decl(const FunctionDecl *decl);
        // ---- 类型参数分派 ----
        // 单态化之后，泛型类 `List<T>` 的实例 `List_str` 里仍然留着按 `T` 分派的
        // 动作调用，例如 `sinter_gen_clone(v)` / `sinter_gen_grow(p, n)`。
        // 类作用域按「实例名后缀」还原类型实参，函数作用域按显式类型实参还原。
        /// 当前所在的泛型实例：模板形参（用于判断 T 在实参里的位置）+ 已代入的 C 类型
        std::vector<TemplateParam> cur_type_params_;
        std::vector<std::string> cur_type_args_;

        /// 当前多态调用的类型标签：`sinter_gen_grow(vals, n, i64)` 末尾的 `i64`
        /// 是编译期标记，只决定归约出的函数名，不进 C 代码、不产生实参。
        std::string tag_token_;
        /// 标签写成模板形参名时（`sinter_gen_text(self.second, B)`）记下它的位置，
        /// 这样 `Pair<A, B>` 的两份实例各自归约到自己的类型实参。
        size_t tag_param_idx_ = static_cast<size_t>(-1);

        /// 该名字是否属于类型参数分派族（`sinter_gen_clone` / `sinter_gen_grow` …）
        static bool is_generic_dispatch_call(const std::string &name);

        /// 多态调用名 → 本作用域下的实际 C 函数名；无法归约时返回空串。
        std::string generic_dispatch_name(const std::string &generic);
        /// 同上，但按第 idx 个类型参数取值（一个类可能有多个 T）。
        std::string generic_dispatch_name_at(const std::string &generic, size_t idx);
        /// 语句里的 `return v` 是否构成助记符调用（如 `return grow(p, n)`）。
        /// 是则把整条 return 改写成「按 T 分派」，返回 true。
        bool emit_dispatch_return(ReturnStmt *r);

        /// 泛型调用的返回类型（按调用点实参代入 T）；非泛型返回空串
        std::string generic_call_return(const FunctionInfo *fi, CallExpr *c);
        std::string upcast_if_needed(Expression *obj, const std::string &want_c_type);
        bool is_subclass_of_type(const std::string &got, const std::string &want);
        std::string current_return_type_;   // 当前生成函数的返回类型（return 向上转型用）
        // 作用域函数（let/run/apply/also/with）调用点推断出的 lambda 类型
        std::vector<std::string> lambda_param_hints_pending_types_;
        /// 当前 lambda 是「接收者函数类型」时的 C 接收者类型（`struct X*`）。
        /// 由调用点按形参类型设置，expr_lambda 据此把接收者加成第一个参数。
        std::string lambda_receiver_type_hint_;
        std::string lambda_return_type_hint_;
        std::string lambda_receiver_;   // 当前 lambda 的接收者形参名（作用域函数）
        int null_safe_depth_ = 0;       // 展开 ?. 时的递归屏蔽计数
        // 语句级临时字符串实参的落地表：表达式 → 局部变量名。
        // 命中时 expr() 直接返回变量名，语句执行完由调用方逐个释放。
        std::unordered_map<const Expression *, std::string> hoist_names_;
        // 复制/移动构造：延迟到所有函数定义之后再生成的替换实现
        std::vector<std::pair<std::string, std::string>> pending_ctor_impls_;
        void emit_ctor_impls();
        std::string c_type(const TypeAnnotation *t) const;
        /// 表达式是否可能改变状态（调用 / 赋值）。字符串模板按出现顺序把
        /// 这类片段落到临时变量，避免 C 的实参求值顺序把它们打乱。
        bool has_side_effect(const Expression *e) const;
        /// 泛型单态化时把类型标注里的类型参数（含函数类型内部）替换成 C 类型。
        /// `(T) -> T` + `T=i64` → `int64_t (*)(int64_t)`；模板原型和定义都必须用它，
        /// 否则会生成字面量 `T`（gcc: unknown type name 'T'）。
        std::string subst_type_annotation(
            const TypeAnnotation *t,
            const std::unordered_map<std::string, std::string> &subst) const;
        std::string c_type_of_expr(Expression *e);

        void collect_imports(const Program &p, std::vector<std::string> &out) const;
        void flatten_module_blocks(Program &p);
        void emit_enum_impl(const EnumDecl *e);
        void emit_class_decl_impl(const ClassDecl *c);
        void emit_alias_impl(const AliasStmt *a);
        void emit_static_fields_impl(const ClassDecl *c);
        void emit_function_decl_impl(const FunctionDecl *f, bool is_method, const std::string &owner);
        /// 可变参数：`fun f(xs: T...)` 的末位形参在 C 侧是 `T *xs, int64_t xs__n`。
        /// 长度形参名固定为 `<参数 C 名>__n`，函数体内可直接引用。
        static std::string varargs_len_name(const std::string &param) {
            return Semantic::to_snake_ident(param) + "__n";
        }
        /// 该函数是否带可变参数（AST 或语义层任一为真）
        static bool fn_is_varargs(const FunctionInfo *fi);
        /// 运算符重载：把 TokenType 转成用户书写的符号（`+`、`==`、`<` …）
        static std::string op_symbol(TokenType t);
        /// 查找可用的二元运算符重载，返回其 C 函数名；没有则返回空串。
        /// 优先匹配左操作数的类型，其次右操作数（支持 `i64 + Vec` 这类写法）。
        std::string find_binary_operator(TokenType op, const std::string &lt, const std::string &rt) const;

        void emit_enum(const EnumDecl *e);
        void emit_class_decl(const ClassDecl *c);
        void emit_alias(const AliasStmt *a);
        void emit_static_fields(const ClassDecl *c);
        // 模块级 val/var：定义进 .c，pub 时在 .h 导出 extern 声明
        void emit_module_global(const VarDeclStmt *v);
        void emit_class_ctor_def(const ClassDecl *c);
        void emit_vtables_impl(const ClassDecl *c);
        void emit_vtables(const ClassDecl *c);

        void emit_function_decl(const FunctionDecl *f, bool is_method, const std::string &owner);
        void emit_function_def(FunctionDecl *f, bool is_method, const std::string &owner);
        /// 泛型单态化：is_method/owner 让类/结构体内的泛型方法也能带上 this 形参
        void emit_function_def_generic(FunctionDecl *f,
                                       const std::unordered_map<std::string, std::string> &subst,
                                       bool is_method = false, const std::string &owner = "");
        void emit_function_proto_generic(FunctionDecl *f,
                                         const std::unordered_map<std::string, std::string> &subst,
                                         bool is_method = false, const std::string &owner = "");


        void emit_program_body(Program &p);

        void stmt(Statement *s);
        void stmt_var(const VarDeclStmt *v);
        void stmt_destructure(const DestructureStmt *d);
        void stmt_return(const ReturnStmt *r);
        void stmt_throw(const ThrowStmt *t);
        void stmt_try(const TryStmt *t);
        void emit_active_finally(bool exit_all, size_t loop_depth);
        /// `return s`（s 是字符串局部变量）时移交所有权，避免返回后立即被释放
        void release_returned_string(const Expression *e);
        void stmt_if(const IfStmt *i);
        void stmt_while(const WhileStmt *w);
        void stmt_for(const ForStmt *f);
        void stmt_do_while(const DoWhileStmt *d);
        /// 发射「每轮重新求值」的循环条件块（临时值 + 条件变量 + 释放 + break），
        /// 返回条件变量名。循环条件里含需要释放的临时字符串时使用。
        std::string emit_loop_cond_block(Expression *cond);
        void stmt_block(const BlockStmt *b);
        void stmt_del(const DelStmt *d);
        void enter_scope();
        void leave_scope(bool no_raii);

        // ---- RAII 清理 ----
        // 作用域退出时对登记的资源实例输出析构，析构后置空保证幂等。
        void emit_scope_exit(size_t idx);
        // 从第 from_depth 层（含）一直清理到最内层，逆序；
        // 用于 return / break / continue 这类提前退出，避免跳过 leave_scope。
        void emit_scope_exits(size_t from_depth, bool no_raii);
        // 登记一个局部对象到当前作用域（由 stmt_var 按最终 C 类型调用）。
        void register_raii(const std::string &name, const std::string &c_name,
                           const std::string &c_type);
        // 按 C 类型反查 Sinter 类名（"struct X*" -> X），三处共用。
        const ClassInfo *class_by_c_type(const std::string &c_type) const;

        std::string expr(Expression *e);
        /// 若表达式是一条模块路径（`std.str`），返回点分路径；否则返回空串。
        std::string module_path_of(const Expression *e) const;
        std::string expr_literal(LiteralExpr *l);
        std::string expr_ident(IdentifierExpr *i);
        std::string expr_binary(const BinaryExpr *b);
        std::string expr_unary(const UnaryExpr *u);
        std::string c_type_short(const TypeAnnotation *t) const;
        CType arg_type_of(Expression *e);
        std::string expr_call(CallExpr *c);
        std::string expr_member(const MemberAccessExpr *m);
        /// `Color.Red` -> `<枚举C名>_<变体>`；不是枚举变体时返回空串。
        std::string enum_variant_c_name(const std::string &enum_name,
                                        const std::string &variant) const;
        std::string expr_index(const IndexAccessExpr *i);
        std::string expr_assign(const AssignmentExpr *a);
        std::string expr_lambda(LambdaExpr *l);
        std::string expr_ternary(const TernaryExpr *t);
        std::string expr_if(const IfExpr *i);
        std::string expr_when(WhenExpr *w);
        std::string expr_array(ArrayLiteralExpr *a);
        /// 用声明的元素 C 类型生成数组字面量（避免从字面量反推时丢掉 int64_t 等宽度）
        std::string expr_array_with(ArrayLiteralExpr *a, const std::string &elem_c);
        std::string expr_group(GroupingExpr *g);
        std::string expr_pipeline(PipelineExpr *p);
        std::string expr_template(StringTemplateExpr *s);
        /// 变量 / 字段 / 下标是借用（调用方后面还要用），不能顺手释放
        static bool is_borrowed_expr(const Expression *e);
        /// 把「调用实参里的临时字符串」提前落到局部变量，返回需要释放的变量名列表。
        /// 每调用一次都会先清空 hoist_names_：表达式文本一旦生成完，映射就没用了，
        /// 留在表里会让下一条语句的同名节点被误替换。
        std::vector<std::string> hoist_temp_str_args(Expression *e);
        /// 收集需要提升的实参节点，**后序**排列（子树先于本层）。
        /// 本层的初始化式会引用子树产出的临时变量，所以子树的声明必须先发。
        void collect_hoist_targets(Expression *e, std::vector<Expression *> &out);
        /// 实参是否值得提升：求值为字符串、不是借用（变量 / 字段 / 下标）、
        /// 也不是没有堆缓冲的字符串字面量。
        /// collect_hoist_targets 与 needs_hoist 共用这一处判定，两边规则必须一致。
        bool is_hoistable_arg(const Expression *e);
        /// 纯谓词：这棵表达式树里有没有需要提升的实参。
        /// 用于在发射语句之前决定生成策略（do-while 要先知道要不要给 continue 发标签）。
        bool needs_hoist(const Expression *e);
        /// 生成 hoist 映射中的实参替换（命中则直接返回临时变量名）
        const std::string *hoist_name_of(const Expression *e) const;
        /// 释放一个已提升的临时字符串并置空（sinter_string_free 只释放 owned 缓冲，
        /// 对字面量视图是无操作，因此不必区分来源）。
        void emit_string_free(const std::string &name);
        /// 收尾：逐个释放本轮提升出来的临时值，并清空替换表。
        /// 清空是必须的——残留的映射会让后面语句里同一节点的文本被替换成
        /// 上一轮的变量名（那个变量早就不在作用域里了）。
        void finish_stmt_temps(const std::vector<std::string> &owned);
        /// 循环体里的 `continue` 该跳到哪里：空串表示发普通 `continue;`。
        /// do-while 的条件需要提升时，条件求值被挪到循环体末尾，
        /// `continue` 必须 goto 到那段代码之前，否则会跳过条件求值（语义改变）。
        std::vector<std::string> loop_continue_labels_;
        /// 该语句子树里（不进入嵌套循环）是否有 `continue`，用于决定标签要不要发。
        static bool has_continue(const Statement *s);
        std::string expr_scope_call(ScopeCallExpr *s);

        std::string str_cast(const std::string &code, const std::string &type_c);
        std::string cstr_cast(const std::string &code, const std::string &type_c);
        std::string class_c_name(const std::string &sinter_name) const;
        const ClassInfo &class_by_name(const std::string &n) const;
        std::string resolve_fn_name(IdentifierExpr *id, std::vector<ExprPtr> *args);
    };
}