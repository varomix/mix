#ifndef SEMA_H
#define SEMA_H

#include "mix.h"
#include "ast.h"
#include "types.h"
#include "symtab.h"

// Generic shape templates kept aside from the regular symbol table; sema
// dispatches them through `instantiate_generic_shape` when a use site
// supplies concrete type args.
typedef struct GenericShapeTemplate {
    char *name;                 // template name (e.g., "Stack")
    AstNode *decl;              // the original shape AST
    struct GenericShapeTemplate *next;
} GenericShapeTemplate;

// Cache of already-instantiated shapes: keyed on mangled name (e.g.,
// "Stack$int"). Saves re-cloning + re-registering on every reference.
typedef struct GenericShapeInstance {
    char *mangled;
    MixType *type;
    struct GenericShapeInstance *next;
} GenericShapeInstance;

typedef struct {
    SymTab symtab;
    Arena *arena;
    // Generic context: type parameter names currently in scope
    char **generic_params;
    int generic_param_count;
    // Conditional compilation
    bool debug_mode;
    // Generic shape templates and instantiation cache
    GenericShapeTemplate *templates;
    GenericShapeInstance *instances;
    // Instantiated shape decls accumulated during sema; appended to the
    // program before codegen so emitters see them as ordinary shapes.
    AstNode **instantiated_decls;
    int instantiated_decl_count;
    int instantiated_decl_cap;
    // While analyzing a method body: the enclosing shape and whether the
    // method is declared mutating (postfix `!`). Used to rewrite bare
    // `field! += val` into `self.field += val`, and to allow field stores
    // through self in mutating methods.
    MixType *current_shape;
    bool current_method_mutates;
    // Every `@const` decl seen by this sema instance — including ones from
    // imported sub-modules. Emitters use this list to inline const values at
    // use sites, which is necessary because the main program AST only sees
    // its own NODE_CONST_DECLs (sub-module decls live in the sub-module AST,
    // which is dropped after compile_module returns).
    AstNode **all_consts;
    int all_const_count;
    int all_const_cap;
    // Module-level mutable variable decls seen across all compiled modules.
    // Spliced into the main program in pass-(e), parallel to all_consts.
    AstNode **all_globals;
    int all_global_count;
    int all_global_cap;
} Sema;

Sema sema_create(Arena *arena);
void sema_analyze(Sema *sema, AstNode *program);

#endif // SEMA_H
