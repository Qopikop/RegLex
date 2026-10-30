/* Abstract syntax tree shared by the C, Java and Python frontends.
 *
 * One node shape for every construct: a kind, an optional name/operator/
 * literal string, an optional declared type, a source position and an
 * ordered list of children. The meaning of each child slot is listed below.
 */
#ifndef AST_H
#define AST_H

#include "util.h"

#define NODE_KINDS(X) \
    X(Program)      /* items...                                              */ \
    X(Function)     /* s=name type=return type; [Params, Block]              */ \
    X(Params)       /* Param...                                              */ \
    X(Param)        /* s=name type; [default value]                          */ \
    X(Class)        /* s=name; members...                                    */ \
    X(StructDef)    /* s=name; VarDecl... (C struct / union)                 */ \
    X(EnumDef)      /* s=name; Enumerator...                                 */ \
    X(Enumerator)   /* s=name; [value]                                       */ \
    X(Block)        /* statements...                                         */ \
    X(DeclGroup)    /* VarDecl...  (int a = 1, b;)                           */ \
    X(VarDecl)      /* s=name type; [init] ; ival = array rank               */ \
    X(If)           /* [cond, then, else?]                                   */ \
    X(While)        /* [cond, body, else?] (Python while/else)               */ \
    X(DoWhile)      /* [body, cond]                                          */ \
    X(For)          /* [init, cond, step, body] (Empty for missing parts)    */ \
    X(ForEach)      /* s=var type; [iterable, body, else?] ; ival=1 if tuple */ \
    X(Return)       /* [value?]                                              */ \
    X(Break)        \
    X(Continue)     \
    X(Pass)         \
    X(ExprStmt)     /* [expr]                                                */ \
    X(Switch)       /* [subject, Case...]                                    */ \
    X(Case)         /* s="default" or [value]; statements follow as kids 1.. */ \
    X(Try)          /* [body, handlers..., finally?]  (not translated)       */ \
    X(Throw)        /* [value?]                                              */ \
    X(Assert)       /* [cond, msg?]                                          */ \
    X(Import)       /* s=text                                                */ \
    X(Directive)    /* s=text (C preprocessor line)                          */ \
    X(Global)       /* s=name (Python global / nonlocal)                     */ \
    X(Unsupported)  /* s=what; a construct parsed but not modelled           */ \
    X(Empty)        \
    X(IntLit)       /* ival; s=source text                                   */ \
    X(FloatLit)     /* fval; s=source text                                   */ \
    X(StrLit)       /* s=decoded value                                       */ \
    X(CharLit)      /* ival                                                  */ \
    X(BoolLit)      /* ival                                                  */ \
    X(NullLit)      \
    X(Name)         /* s=identifier                                          */ \
    X(Binary)       /* s=op; [lhs, rhs]                                      */ \
    X(Logical)      /* s="&&"/"||"/"and"/"or"; [lhs, rhs]                    */ \
    X(Compare)      /* s="op1,op2,..."; [e0, e1, e2...] (Python chains)      */ \
    X(Unary)        /* s=op; [operand]                                       */ \
    X(IncDec)       /* s="++"/"--"; ival=1 prefix 0 postfix; [target]        */ \
    X(Assign)       /* s="=" "+=" ...; [target, value]                       */ \
    X(Ternary)      /* [cond, then, else]                                    */ \
    X(Call)         /* [callee, args...]                                     */ \
    X(Index)        /* [base, index]                                         */ \
    X(Member)       /* s=field; [object] ; ival=1 for '->'                   */ \
    X(Cast)         /* type; [operand]                                       */ \
    X(NewArray)     /* type=element type; [dim exprs...] or [ArrayInit]      */ \
    X(NewObject)    /* s=class; [args...]                                    */ \
    X(ArrayInit)    /* elements... (C/Java {1,2,3}, Python [1,2,3])          */ \
    X(Tuple)        /* elements...                                           */ \
    X(Dict)         /* key, value, key, value...                             */ \
    X(Slice)        /* [lo, hi, step] (Empty when missing)                   */ \
    X(KwArg)        /* s=name; [value]                                       */ \
    X(Star)         /* [value]  (*args)                                      */ \
    X(Lambda)       /* [params, body]                                        */ \
    X(Comprehension)/* s="list"/"dict"/"set"/"gen"; [element, ...]           */ \
    X(FString)      /* parts: StrLit or FormatPart                           */ \
    X(FormatPart)   /* s=format spec or NULL; [expr]                         */ \
    X(Sizeof)       /* type or [expr]                                        */

typedef enum {
#define X(k) N_##k,
    NODE_KINDS(X)
#undef X
    N__COUNT
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind kind;
    const char *s;       /* name, operator or literal value (interned) */
    const char *type;    /* declared type text, e.g. "int", "int[]", "String" */
    int line, col;
    long long ival;
    double fval;
    int flags;
    VEC(Node *) k;
};

enum { NF_STATIC = 1, NF_CONST = 2, NF_POINTER = 4, NF_UNSIGNED = 8, NF_PROTOTYPE = 16, NF_METHOD = 32 };

Node *node_new(NodeKind kind, int line, int col);
void  node_add(Node *parent, Node *child);
Node *node_kid(const Node *n, int i);    /* NULL if out of range */
const char *node_kind_name(NodeKind k);
int   node_count(const Node *n);
void  node_free(Node *n);

/* After error recovery a node can miss children the later stages index
 * directly (a do-loop without its condition...). This fills every required
 * slot with an Empty node so semantic analysis and lowering stay safe. */
void  ast_fill_required(Node *n, int is_python);

/* JSON tree for the web page; nodes past max_nodes are summarised. */
void  ast_json(Buf *b, const Node *n, int *budget);

#endif
