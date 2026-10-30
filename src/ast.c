#include "ast.h"

static const char *kind_names[] = {
#define X(k) #k,
    NODE_KINDS(X)
#undef X
};

Node *node_new(NodeKind kind, int line, int col) {
    Node *n = xcalloc(1, sizeof(Node));
    n->kind = kind; n->line = line; n->col = col;
    return n;
}

void node_add(Node *parent, Node *child) { vpush(parent->k, child); }
Node *node_kid(const Node *n, int i) { return (n && i >= 0 && i < n->k.n) ? n->k.v[i] : NULL; }
const char *node_kind_name(NodeKind k) { return k < N__COUNT ? kind_names[k] : "?"; }

void ast_fill_required(Node *n, int is_python) {
    if (!n) return;
    int need = 0;
    switch (n->kind) {
    case N_If: case N_While: case N_DoWhile: case N_Binary: case N_Logical: case N_Assign: case N_Index:
    case N_Compare: case N_Switch: need = n->kind == N_Switch ? 1 : 2; break;
    case N_Ternary: case N_Slice: need = 3; break;
    case N_For: need = 4; break;
    case N_ForEach: need = is_python ? 3 : 2; break;
    case N_Unary: case N_IncDec: case N_Member: case N_Cast: case N_Call: case N_VarDecl: case N_Case:
    case N_FormatPart: case N_KwArg: case N_Star: case N_ExprStmt: case N_Throw: case N_Assert: need = 1; break;
    case N_Function: need = (n->flags & NF_PROTOTYPE) ? 1 : 2; break;
    case N_Try: need = 1; break;
    default: break;
    }
    for (int i = 0; i < n->k.n; i++) {
        if (!n->k.v[i]) n->k.v[i] = node_new(N_Empty, n->line, n->col);
        ast_fill_required(n->k.v[i], is_python);
    }
    while (n->k.n < need) {
        NodeKind k = N_Empty;
        if (n->kind == N_Function && n->k.n == 0) k = N_Params;
        else if (n->kind == N_Function || ((n->kind == N_If || n->kind == N_While || n->kind == N_DoWhile || n->kind == N_For) && n->k.n == need - 1)) k = N_Block;
        if (n->kind == N_DoWhile && n->k.n == 0) k = N_Block;
        node_add(n, node_new(k, n->line, n->col));
    }
}

int node_count(const Node *n) {
    if (!n) return 0;
    int c = 1;
    for (int i = 0; i < n->k.n; i++) c += node_count(n->k.v[i]);
    return c;
}

void node_free(Node *n) {
    if (!n) return;
    for (int i = 0; i < n->k.n; i++) node_free(n->k.v[i]);
    vfree(n->k);
    free(n);
}

/* Short label the tree viewer shows on a node. */
static void label(Buf *b, const Node *n) {
    switch (n->kind) {
    case N_IntLit: case N_FloatLit: bputs(b, n->s ? n->s : "?"); break;
    case N_StrLit: {
        Buf t = {0};
        bputc(&t, '"');
        size_t len = n->s ? strlen(n->s) : 0;
        bput(&t, n->s ? n->s : "", len > 24 ? 24 : len);
        if (len > 24) bputs(&t, "\xe2\x80\xa6");
        bputc(&t, '"');
        bjson_cstr(b, t.s); free(t.s);
        return;
    }
    case N_CharLit: bprintf(b, "'%c'", (int)(n->ival >= 32 && n->ival < 127 ? n->ival : '?')); break;
    case N_BoolLit: bputs(b, n->ival ? "true" : "false"); break;
    default:
        if (n->s) bputs(b, n->s);
    }
}

void ast_json(Buf *b, const Node *n, int *budget) {
    if (!n) { bputs(b, "null"); return; }
    (*budget)--;
    bprintf(b, "{\"kind\":\"%s\",\"line\":%d,\"col\":%d", node_kind_name(n->kind), n->line, n->col);
    Buf l = {0};
    label(&l, n);
    if (l.n) {
        bputs(b, ",\"label\":");
        if (l.s[0] == '"' && n->kind == N_StrLit) bputs(b, l.s); else bjson_cstr(b, l.s);
    }
    free(l.s);
    if (n->type) { bputs(b, ",\"type\":"); bjson_cstr(b, n->type); }
    if (n->k.n) {
        bputs(b, ",\"kids\":[");
        int shown = 0;
        for (int i = 0; i < n->k.n; i++) {
            if (*budget <= 0) {
                bprintf(b, "%s{\"kind\":\"Truncated\",\"label\":\"%d more children\",\"line\":0,\"col\":0}",
                        shown ? "," : "", n->k.n - i);
                break;
            }
            if (shown++) bputc(b, ',');
            ast_json(b, n->k.v[i], budget);
        }
        bputc(b, ']');
    }
    bputc(b, '}');
}
