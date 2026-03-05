#ifndef AST_H
#define AST_H

#include "mix.h"
#include "token.h"

typedef enum {
    // Top-level
    NODE_PROGRAM,
    NODE_FN_DECL,
    NODE_EXTERN_BLOCK,
    NODE_EXTERN_FN_DECL,
    NODE_USE_DECL,

    // Statements
    NODE_BLOCK,
    NODE_VAR_DECL,
    NODE_ASSIGN,
    NODE_IF_STMT,
    NODE_WHILE_STMT,
    NODE_FOR_STMT,
    NODE_MATCH_STMT,
    NODE_DONE_STMT,
    NODE_BREAK_STMT,
    NODE_CONTINUE_STMT,
    NODE_EXPR_STMT,
    NODE_DEFER_STMT,
    NODE_UNSAFE_BLOCK,      // unsafe INDENT block DEDENT
    NODE_ZONE_STMT,         // zone [:name] INDENT block DEDENT
    NODE_DEREF_ASSIGN,      // *ptr = val (inside unsafe)
    NODE_FAIL_STMT,         // fail "message" or fail ErrorShape.Variant(...)

    // Top-level
    NODE_CONST_DECL,

    // Expressions
    NODE_INT_LIT,
    NODE_FLOAT_LIT,
    NODE_STRING_LIT,
    NODE_STRING_INTERP,  // "Hello {name}!" → parts + expressions
    NODE_BOOL_LIT,
    NODE_NONE_LIT,
    NODE_IDENT,
    NODE_BINARY_EXPR,
    NODE_UNARY_EXPR,
    NODE_CALL_EXPR,
    NODE_LAMBDA,            // x => x * 2
    NODE_LIST_LIT,          // [1, 2, 3]
    NODE_MAP_LIT,           // {"key": val, ...}
    NODE_INDEX_EXPR,        // list[i]
    NODE_SLICE_EXPR,        // list[start..end]
    NODE_LIST_COMP,         // [expr for x in iter if cond]
    NODE_ELSE_EXPR,         // expr else default
    NODE_SET_LIT,           // set{1, 2, 3}
    NODE_CAST_EXPR,         // int32(expr), uint8(expr)

    // Shape-related
    NODE_SHAPE_DECL,
    NODE_FIELD_EXPR,        // obj.field
    NODE_METHOD_CALL,       // obj.method(args)
    NODE_SHAPE_LIT,         // Vec2(x: 1.0, y: 2.0)
    NODE_FIELD_ASSIGN,      // obj.field! = expr
    NODE_INDEX_ASSIGN,      // obj[idx] = expr

    // Top-level
    NODE_TYPE_ALIAS,        // type Id = int
    NODE_COND_DECL,         // @os == "macos" INDENT decls DEDENT

    // Type nodes
    NODE_TYPE_NAME,
    NODE_TYPE_PTR,
    NODE_TYPE_OPTIONAL,     // T?

    // Concurrency
    NODE_SHARED_EXPR,       // shared int(0)
    NODE_GO_EXPR,           // go compute(5)
    NODE_WAIT_EXPR,         // wait t
    NODE_TRY_EXPR,          // expr? — unwrap result, propagate error
} NodeKind;

typedef struct ShapeField {
    char *name;
    AstNode *type;
    int offset;
    int size;
} ShapeField;

typedef struct Param {
    char *name;
    AstNode *type;
    bool is_mutable;
} Param;

typedef struct ShapeVariantDecl {
    char *name;
    Param *fields;
    int field_count;
} ShapeVariantDecl;

struct AstNode {
    NodeKind kind;
    SrcLoc loc;
    MixType *resolved_type;

    union {
        // NODE_PROGRAM
        struct { AstNode **decls; int decl_count; } program;

        // NODE_FN_DECL
        struct {
            char *name;
            Param *params;
            int param_count;
            AstNode *return_type;
            AstNode *body;
            bool has_side_effects;  // ~
            bool has_mutation;      // !
            bool is_pub;            // pub
            // Generics
            char **type_params;     // @T, @K → ["T", "K"]
            int type_param_count;
            // Constraints: @T has +, ==
            char **constraints;     // ["+", "==", "area"]
            int constraint_count;
        } fn_decl;

        // NODE_EXTERN_BLOCK
        struct {
            char *lib_name;
            AstNode **decls;
            int decl_count;
        } extern_block;

        // NODE_EXTERN_FN_DECL
        struct {
            char *name;
            Param *params;
            int param_count;
            AstNode *return_type;
            bool has_side_effects;
            bool has_mutation;
            bool is_variadic;
        } extern_fn_decl;

        // NODE_BLOCK
        struct { AstNode **stmts; int stmt_count; } block;

        // NODE_VAR_DECL
        struct {
            char *name;
            AstNode *type_ann;
            AstNode *init_expr;
            bool is_mutable;
        } var_decl;

        // NODE_ASSIGN
        struct {
            char *name;
            TokenKind op;       // TOK_EQ, TOK_PLUS_EQ, etc.
            AstNode *value;
        } assign;

        // NODE_IF_STMT
        struct {
            AstNode *condition;
            AstNode *then_block;
            AstNode *else_block;
        } if_stmt;

        // NODE_WHILE_STMT
        struct {
            AstNode *condition;
            AstNode *body;
        } while_stmt;

        // NODE_MATCH_STMT
        struct {
            AstNode *subject;
            struct MatchArm {
                AstNode *pattern;    // expression to compare (or NULL for _)
                bool is_wildcard;    // true if _ pattern
                AstNode *body;       // single expr or block
            } *arms;
            int arm_count;
        } match_stmt;

        // NODE_FOR_STMT
        struct {
            char *var_name;
            char *index_name;   // NULL if no index var
            AstNode *iterable;
            AstNode *body;
        } for_stmt;

        // NODE_DONE_STMT
        struct { AstNode *value; } done_stmt;

        // NODE_EXPR_STMT
        struct { AstNode *expr; } expr_stmt;

        // NODE_DEFER_STMT
        struct { AstNode *stmt; } defer_stmt;

        // NODE_UNSAFE_BLOCK
        struct { AstNode *body; } unsafe_block;

        // NODE_ZONE_STMT
        struct {
            char *name;         // NULL for anonymous zone
            AstNode *body;
        } zone_stmt;

        // NODE_DEREF_ASSIGN — *ptr = val
        struct {
            AstNode *ptr_expr;
            AstNode *value;
        } deref_assign;

        // NODE_FAIL_STMT — fail expr
        struct { AstNode *value; } fail_stmt;

        // NODE_CONST_DECL
        struct {
            char *name;
            AstNode *value;
            bool is_pub;
        } const_decl;

        // NODE_USE_DECL — use path.to.module  OR  use alias path.to.module
        struct {
            char *module_path;   // "engine.physics" or "math"
            char *alias;         // NULL if no alias, or the alias name
            char **imports;      // selective: use math: add, PI (NULL if import all)
            int import_count;
        } use_decl;

        // NODE_INT_LIT
        struct { int64_t value; } int_lit;

        // NODE_FLOAT_LIT
        struct { double value; } float_lit;

        // NODE_STRING_LIT
        struct { char *value; int length; } string_lit;

        // NODE_STRING_INTERP — "text {expr} text {expr} text"
        // parts[0], exprs[0], parts[1], exprs[1], ..., parts[n]
        // Always has one more part than expression
        struct {
            char **parts;       // string segments between {}'s
            int *part_lengths;
            AstNode **exprs;    // expressions inside {}'s
            int expr_count;     // number of expressions
        } string_interp;

        // NODE_BOOL_LIT
        struct { bool value; } bool_lit;

        // NODE_IDENT
        struct { char *name; bool is_mutable; } ident;

        // NODE_BINARY_EXPR
        struct { TokenKind op; AstNode *left; AstNode *right; } binary;

        // NODE_UNARY_EXPR
        struct { TokenKind op; AstNode *operand; } unary;

        // NODE_CALL_EXPR
        struct {
            char *name;
            AstNode **args;
            int arg_count;
            bool is_mutable_call;  // call with !
        } call;

        // NODE_LAMBDA — x => expr  OR  (a, b) => expr
        struct {
            char **param_names;
            int param_count;
            AstNode *body;         // expression
            char *generated_name;  // filled by emitter: $lambda_0, $lambda_1...
        } lambda;

        // NODE_LIST_LIT — [expr, expr, ...]
        struct {
            AstNode **elements;
            int element_count;
        } list_lit;

        // NODE_MAP_LIT — {"key": val, ...}
        struct {
            AstNode **keys;
            AstNode **values;
            int entry_count;
        } map_lit;

        // NODE_INDEX_EXPR — list[index]
        struct {
            AstNode *object;
            AstNode *index;
        } index_expr;

        // NODE_SLICE_EXPR — list[start..end]
        struct {
            AstNode *object;
            AstNode *start;     // NULL if omitted (..end)
            AstNode *end;       // NULL if omitted (start..)
            bool inclusive;      // true for ..=
        } slice_expr;

        // NODE_LIST_COMP — [expr for var in iter if cond]
        struct {
            AstNode *expr;
            char *var_name;
            AstNode *iterable;
            AstNode *condition;     // NULL if no filter
        } list_comp;

        // NODE_ELSE_EXPR — expr else default
        struct {
            AstNode *value;
            AstNode *fallback;
        } else_expr;

        // NODE_SET_LIT — set{expr, expr, ...}
        struct {
            AstNode **elements;
            int element_count;
        } set_lit;

        // NODE_CAST_EXPR — int32(expr), uint8(expr)
        struct {
            TokenKind target_type;
            AstNode *value;
        } cast_expr;

        // NODE_TYPE_ALIAS — type Name = Type
        struct {
            char *name;
            AstNode *target_type;
            bool is_pub;
        } type_alias;

        // NODE_COND_DECL — @os == "macos" INDENT decls DEDENT
        struct {
            char *condition_name;   // "os", "arch", "debug", "release"
            char *condition_value;  // "macos", "linux", "aarch64", NULL for boolean
            AstNode **decls;
            int decl_count;
            bool active;            // set by sema after evaluation
        } cond_decl;

        // NODE_SHAPE_DECL
        struct {
            char *name;
            ShapeField *fields;
            int field_count;
            AstNode **methods;
            int method_count;
            ShapeVariantDecl *variants;
            int variant_count;
            bool is_pub;
        } shape_decl;

        // NODE_FIELD_EXPR  (obj.field)
        struct {
            AstNode *object;
            char *field_name;
        } field_expr;

        // NODE_METHOD_CALL (obj.method(args))
        struct {
            AstNode *object;
            char *method_name;
            AstNode **args;
            int arg_count;
        } method_call;

        // NODE_SHAPE_LIT  (Vec2(x: 1.0, y: 2.0))
        struct {
            char *shape_name;
            char **field_names;
            AstNode **field_values;
            int field_count;
        } shape_lit;

        // NODE_FIELD_ASSIGN (obj.field = expr)
        struct {
            AstNode *object;
            char *field_name;
            TokenKind op;
            AstNode *value;
        } field_assign;

        // NODE_INDEX_ASSIGN (obj[idx] = expr)
        struct {
            AstNode *object;
            AstNode *index;
            AstNode *value;
        } index_assign;

        // NODE_TYPE_NAME
        struct { char *name; TokenKind type_kind; } type_name;

        // NODE_TYPE_PTR
        struct { AstNode *base_type; } type_ptr;

        // NODE_TYPE_OPTIONAL
        struct { AstNode *inner_type; } type_optional;

        // NODE_SHARED_EXPR — shared int(0)
        struct { AstNode *init_expr; } shared_expr;

        // NODE_GO_EXPR — go compute(5)
        struct { AstNode *call_expr; } go_expr;

        // NODE_WAIT_EXPR — wait t
        struct { AstNode *handle_expr; } wait_expr;

        // NODE_TRY_EXPR — expr?
        struct { AstNode *expr; } try_expr;
    };
};

// AST constructors (use arena)
AstNode *ast_new(Arena *a, NodeKind kind, SrcLoc loc);
void ast_print(AstNode *node, int indent);

#endif // AST_H
