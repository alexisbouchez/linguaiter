#define _POSIX_C_SOURCE 200809L
#include "codegen.h"
#include "codegen/codegen_internal.h"
#include "diagnostic.h"
#include "import.h"
#include <libgen.h>
#include <string.h>

/* If no target backend matched, fail at link time with a clear message. */
#if !defined(__linux__) || !defined(__x86_64__)
#if !defined(__APPLE__) || !defined(__aarch64__)
/* Provide a stub so the compiler still reports the real problem. */
int emit_binary(int string_count, int *str_offsets, int *str_lengths_arr,
                Buffer *strings, const char *output_path)
{
    (void)string_count; (void)str_offsets; (void)str_lengths_arr;
    (void)strings; (void)output_path;
    diag_error_no_loc("no codegen backend for this platform");
    return 1;
}
#endif
#endif

/* ================================================================
 * Function table
 * ================================================================ */

typedef struct {
    char *name;
    ASTNode *decl;
} FnEntry;

typedef struct {
    FnEntry *entries;
    int count;
    int cap;
    char **evaluating;
    int eval_count;
    int eval_cap;
} FnTable;

static void fn_table_init(FnTable *ft) {
    ft->cap = 8;
    ft->count = 0;
    ft->entries = malloc(ft->cap * sizeof(FnEntry));
    ft->eval_cap = 8;
    ft->eval_count = 0;
    ft->evaluating = malloc(ft->eval_cap * sizeof(char *));
}

static void fn_table_free(FnTable *ft) {
    free(ft->entries);
    free(ft->evaluating);
}

static FnEntry *fn_table_find(FnTable *ft, const char *name) {
    for (int i = 0; i < ft->count; i++)
        if (strcmp(name, ft->entries[i].name) == 0)
            return &ft->entries[i];
    return NULL;
}

static void fn_table_add(FnTable *ft, char *name, ASTNode *decl) {
    if (ft->count == ft->cap) {
        ft->cap *= 2;
        ft->entries = realloc(ft->entries, ft->cap * sizeof(FnEntry));
    }
    ft->entries[ft->count].name = name;
    ft->entries[ft->count].decl = decl;
    ft->count++;
}

/* ================================================================
 * Dynamic print list
 * ================================================================ */

typedef struct {
    char **strings;
    int *lengths;
    int count;
    int cap;
} PrintList;

static void print_list_init(PrintList *pl) {
    pl->cap = 16;
    pl->count = 0;
    pl->strings = malloc(pl->cap * sizeof(char *));
    pl->lengths = malloc(pl->cap * sizeof(int));
}

static void print_list_free(PrintList *pl) {
    free(pl->strings);
    free(pl->lengths);
}

static void print_list_add(PrintList *pl, const char *str, int len) {
    if (pl->count == pl->cap) {
        pl->cap *= 2;
        pl->strings = realloc(pl->strings, pl->cap * sizeof(char *));
        pl->lengths = realloc(pl->lengths, pl->cap * sizeof(int));
    }
    pl->strings[pl->count] = (char *)str;
    pl->lengths[pl->count] = len;
    pl->count++;
}

/* ================================================================
 * ObjData — compile-time object instance
 * ================================================================ */

typedef struct {
    char *class_name;
    char **field_names;
    int field_count;
    /* field_values is an array of EvalResult (forward declared below) */
    struct EvalResultS *field_values;
} ObjData;

/* ================================================================
 * EvalResult — compile-time evaluated value
 * ================================================================ */

typedef struct EvalResultS {
    ValueType type;
    long int_val;
    double float_val;
    char *str_val;
    int str_len;
    int bool_val;
    ObjData *obj_val;
} EvalResult;

/* ================================================================
 * ClassDef — compile-time class definition
 * ================================================================ */

typedef struct {
    char *name;
    char *parent_name;
    char **field_names;
    ValueType *field_types;
    int field_count;
    ASTNode *methods; /* linked list of NODE_FN_DECL */
    SourceLoc loc;
} ClassDef;

typedef struct {
    ClassDef *entries;
    int count;
    int cap;
} ClassTable;

static void class_table_init(ClassTable *ct) {
    ct->cap = 8;
    ct->count = 0;
    ct->entries = malloc(ct->cap * sizeof(ClassDef));
}

static void class_table_free(ClassTable *ct) {
    for (int i = 0; i < ct->count; i++) {
        free(ct->entries[i].field_names);
        free(ct->entries[i].field_types);
    }
    free(ct->entries);
}

static ClassDef *class_table_find(ClassTable *ct, const char *name) {
    for (int i = 0; i < ct->count; i++)
        if (strcmp(name, ct->entries[i].name) == 0)
            return &ct->entries[i];
    return NULL;
}


/* ================================================================
 * Symbol table with parent chain for block scoping
 * ================================================================ */

typedef struct {
    char *name;
    EvalResult val;
    int is_const;
    int mutated;
    SourceLoc loc;
} Symbol;

typedef struct SymTable {
    Symbol *syms;
    int count;
    int cap;
    struct SymTable *parent;
} SymTable;

static void sym_table_init(SymTable *st) {
    st->cap = 16;
    st->count = 0;
    st->syms = malloc(st->cap * sizeof(Symbol));
    st->parent = NULL;
}

static void sym_table_free(SymTable *st) {
    free(st->syms);
}

/* Lookup in current scope only */
static int sym_lookup(SymTable *st, const char *name) {
    for (int i = 0; i < st->count; i++)
        if (strcmp(name, st->syms[i].name) == 0) return i;
    return -1;
}

/* Walk parent chain to find a symbol */
static Symbol *sym_find(SymTable *st, const char *name) {
    for (SymTable *s = st; s; s = s->parent) {
        int idx = sym_lookup(s, name);
        if (idx >= 0) return &s->syms[idx];
    }
    return NULL;
}

static void sym_add(SymTable *st, const char *name, EvalResult val, int is_const, SourceLoc loc) {
    if (st->count == st->cap) {
        st->cap *= 2;
        st->syms = realloc(st->syms, st->cap * sizeof(Symbol));
    }
    st->syms[st->count].name = (char *)name;
    st->syms[st->count].val = val;
    st->syms[st->count].is_const = is_const;
    st->syms[st->count].mutated = 0;
    st->syms[st->count].loc = loc;
    st->count++;
}

/* ================================================================
 * Expression evaluation (single SymTable with parent chain)
 * ================================================================ */

/* Global codegen context for use by eval_expr (EXPR_FN_CALL) */
static FnTable *g_ft;
static ClassTable *g_ct;
static PrintList *g_prints;

/* ================================================================
 * Standard library import tracking
 * ================================================================ */

static const char *g_stdlib_string_fns[] = {
    "len", "trim", "contains", "replace", "to_upper", "to_lower",
    "starts_with", "ends_with", "index_of", "char_at", "substr"
};
#define STDLIB_STRING_FN_COUNT 11
static char g_stdlib_imported_flags[STDLIB_STRING_FN_COUNT];

static void stdlib_reset(void) {
    memset(g_stdlib_imported_flags, 0, sizeof(g_stdlib_imported_flags));
}

static int stdlib_fn_index(const char *name) {
    for (int i = 0; i < STDLIB_STRING_FN_COUNT; i++)
        if (strcmp(g_stdlib_string_fns[i], name) == 0) return i;
    return -1;
}

static int stdlib_fn_is_imported(const char *name) {
    int idx = stdlib_fn_index(name);
    return idx >= 0 && g_stdlib_imported_flags[idx];
}

static void stdlib_fn_import(const char *name) {
    int idx = stdlib_fn_index(name);
    if (idx >= 0) g_stdlib_imported_flags[idx] = 1;
}

static EvalResult eval_expr(Expr *expr, SymTable *st);
static char *eval_to_string(EvalResult *r, int *out_len);
static int evaluate_fn_call(FnTable *ft, ClassTable *ct, SymTable *outer_st,
                            const char *fn_name, SourceLoc call_loc,
                            int arg_count,
                            char **arg_values, int *arg_lengths,
                            ValueType *arg_types,
                            char **arg_names,
                            char **ret_value, int *ret_len, ValueType *ret_type,
                            PrintList *prints);
static EvalResult evaluate_method_call(ASTNode *n, SymTable *st, FnTable *ft,
                                       ClassTable *ct, PrintList *prints,
                                       int require_value);

static EvalResult eval_binary(BinOpKind op, EvalResult lhs, EvalResult rhs, SourceLoc loc) {
    EvalResult r;
    memset(&r, 0, sizeof(r));

    /* Logical operators */
    if (op == BINOP_AND || op == BINOP_OR) {
        if (lhs.type != VAL_BOOL)
            diag_emit(loc, DIAG_ERROR, "left operand of '%s' is not a bool",
                      op == BINOP_AND ? "and" : "or");
        if (rhs.type != VAL_BOOL)
            diag_emit(loc, DIAG_ERROR, "right operand of '%s' is not a bool",
                      op == BINOP_AND ? "and" : "or");
        r.type = VAL_BOOL;
        r.bool_val = (op == BINOP_AND) ? (lhs.bool_val && rhs.bool_val) : (lhs.bool_val || rhs.bool_val);
        return r;
    }

    /* Comparison operators */
    if (op >= BINOP_EQ && op <= BINOP_LE) {
        /* Type promotion for comparisons: int op float → float */
        if ((lhs.type == VAL_INT && rhs.type == VAL_FLOAT) ||
            (lhs.type == VAL_FLOAT && rhs.type == VAL_INT)) {
            double a = (lhs.type == VAL_INT) ? (double)lhs.int_val : lhs.float_val;
            double b = (rhs.type == VAL_INT) ? (double)rhs.int_val : rhs.float_val;
            r.type = VAL_BOOL;
            switch (op) {
                case BINOP_EQ: r.bool_val = a == b; break;
                case BINOP_NE: r.bool_val = a != b; break;
                case BINOP_GT: r.bool_val = a > b; break;
                case BINOP_GE: r.bool_val = a >= b; break;
                case BINOP_LT: r.bool_val = a < b; break;
                case BINOP_LE: r.bool_val = a <= b; break;
                default: break;
            }
            return r;
        }

        if (lhs.type != rhs.type)
            diag_emit(loc, DIAG_ERROR, "cannot compare '%s' with '%s'",
                      value_type_name(lhs.type), value_type_name(rhs.type));

        r.type = VAL_BOOL;
        if (lhs.type == VAL_INT) {
            switch (op) {
                case BINOP_EQ: r.bool_val = lhs.int_val == rhs.int_val; break;
                case BINOP_NE: r.bool_val = lhs.int_val != rhs.int_val; break;
                case BINOP_GT: r.bool_val = lhs.int_val > rhs.int_val; break;
                case BINOP_GE: r.bool_val = lhs.int_val >= rhs.int_val; break;
                case BINOP_LT: r.bool_val = lhs.int_val < rhs.int_val; break;
                case BINOP_LE: r.bool_val = lhs.int_val <= rhs.int_val; break;
                default: break;
            }
        } else if (lhs.type == VAL_FLOAT) {
            switch (op) {
                case BINOP_EQ: r.bool_val = lhs.float_val == rhs.float_val; break;
                case BINOP_NE: r.bool_val = lhs.float_val != rhs.float_val; break;
                case BINOP_GT: r.bool_val = lhs.float_val > rhs.float_val; break;
                case BINOP_GE: r.bool_val = lhs.float_val >= rhs.float_val; break;
                case BINOP_LT: r.bool_val = lhs.float_val < rhs.float_val; break;
                case BINOP_LE: r.bool_val = lhs.float_val <= rhs.float_val; break;
                default: break;
            }
        } else if (lhs.type == VAL_STRING) {
            int cmp = strcmp(lhs.str_val, rhs.str_val);
            switch (op) {
                case BINOP_EQ: r.bool_val = cmp == 0; break;
                case BINOP_NE: r.bool_val = cmp != 0; break;
                case BINOP_GT: r.bool_val = cmp > 0; break;
                case BINOP_GE: r.bool_val = cmp >= 0; break;
                case BINOP_LT: r.bool_val = cmp < 0; break;
                case BINOP_LE: r.bool_val = cmp <= 0; break;
                default: break;
            }
        } else if (lhs.type == VAL_BOOL) {
            if (op != BINOP_EQ && op != BINOP_NE)
                diag_emit(loc, DIAG_ERROR, "ordering comparisons not supported for bool");
            r.bool_val = (op == BINOP_EQ) ? (lhs.bool_val == rhs.bool_val) : (lhs.bool_val != rhs.bool_val);
        }
        return r;
    }

    /* Bitwise operators (int-only) */
    if (op == BINOP_BIT_AND || op == BINOP_BIT_OR || op == BINOP_BIT_XOR ||
        op == BINOP_SHL || op == BINOP_SHR) {
        if (lhs.type != VAL_INT)
            diag_emit(loc, DIAG_ERROR, "bitwise operator requires int operands, got '%s'", value_type_name(lhs.type));
        if (rhs.type != VAL_INT)
            diag_emit(loc, DIAG_ERROR, "bitwise operator requires int operands, got '%s'", value_type_name(rhs.type));
        r.type = VAL_INT;
        switch (op) {
            case BINOP_BIT_AND: r.int_val = lhs.int_val & rhs.int_val; break;
            case BINOP_BIT_OR:  r.int_val = lhs.int_val | rhs.int_val; break;
            case BINOP_BIT_XOR: r.int_val = lhs.int_val ^ rhs.int_val; break;
            case BINOP_SHL:     r.int_val = lhs.int_val << rhs.int_val; break;
            case BINOP_SHR:     r.int_val = lhs.int_val >> rhs.int_val; break;
            default: break;
        }
        return r;
    }

    /* Arithmetic operators */

    /* String concatenation with + (auto-convert non-string operand) */
    if (op == BINOP_ADD && (lhs.type == VAL_STRING || rhs.type == VAL_STRING)) {
        /* Auto-convert non-string operand to string */
        if (lhs.type != VAL_STRING) {
            int tmp_len;
            char *tmp = eval_to_string(&lhs, &tmp_len);
            lhs.type = VAL_STRING;
            lhs.str_val = tmp;
            lhs.str_len = tmp_len;
        }
        if (rhs.type != VAL_STRING) {
            int tmp_len;
            char *tmp = eval_to_string(&rhs, &tmp_len);
            rhs.type = VAL_STRING;
            rhs.str_val = tmp;
            rhs.str_len = tmp_len;
        }
        r.type = VAL_STRING;
        r.str_len = lhs.str_len + rhs.str_len;
        r.str_val = malloc(r.str_len + 1);
        memcpy(r.str_val, lhs.str_val, lhs.str_len);
        memcpy(r.str_val + lhs.str_len, rhs.str_val, rhs.str_len);
        r.str_val[r.str_len] = '\0';
        return r;
    }

    /* Type checking for arithmetic */
    if (lhs.type != VAL_INT && lhs.type != VAL_FLOAT)
        diag_emit(loc, DIAG_ERROR, "arithmetic not supported for '%s'", value_type_name(lhs.type));
    if (rhs.type != VAL_INT && rhs.type != VAL_FLOAT)
        diag_emit(loc, DIAG_ERROR, "arithmetic not supported for '%s'", value_type_name(rhs.type));

    /* Modulo: int only */
    if (op == BINOP_MOD) {
        if (lhs.type != VAL_INT || rhs.type != VAL_INT)
            diag_emit(loc, DIAG_ERROR, "'%%' operator requires int operands");
        if (rhs.int_val == 0)
            diag_emit(loc, DIAG_ERROR, "division by zero");
        r.type = VAL_INT;
        r.int_val = lhs.int_val % rhs.int_val;
        return r;
    }

    /* Type promotion: int op float → float */
    if (lhs.type == VAL_FLOAT || rhs.type == VAL_FLOAT) {
        double a = (lhs.type == VAL_INT) ? (double)lhs.int_val : lhs.float_val;
        double b = (rhs.type == VAL_INT) ? (double)rhs.int_val : rhs.float_val;
        r.type = VAL_FLOAT;
        switch (op) {
            case BINOP_ADD: r.float_val = a + b; break;
            case BINOP_SUB: r.float_val = a - b; break;
            case BINOP_MUL: r.float_val = a * b; break;
            case BINOP_DIV:
                if (b == 0.0)
                    diag_emit(loc, DIAG_ERROR, "division by zero");
                r.float_val = a / b;
                break;
            default: break;
        }
        return r;
    }

    /* Both int */
    r.type = VAL_INT;
    switch (op) {
        case BINOP_ADD: r.int_val = lhs.int_val + rhs.int_val; break;
        case BINOP_SUB: r.int_val = lhs.int_val - rhs.int_val; break;
        case BINOP_MUL: r.int_val = lhs.int_val * rhs.int_val; break;
        case BINOP_DIV:
            if (rhs.int_val == 0)
                diag_emit(loc, DIAG_ERROR, "division by zero");
            r.int_val = lhs.int_val / rhs.int_val;
            break;
        default: break;
    }
    return r;
}

static EvalResult eval_expr(Expr *expr, SymTable *st) {
    EvalResult r;
    memset(&r, 0, sizeof(r));

    switch (expr->kind) {
        case EXPR_INT_LIT:
            r.type = VAL_INT;
            r.int_val = expr->as.int_lit.value;
            return r;
        case EXPR_FLOAT_LIT:
            r.type = VAL_FLOAT;
            r.float_val = expr->as.float_lit.value;
            return r;
        case EXPR_STRING_LIT:
            r.type = VAL_STRING;
            r.str_val = expr->as.string_lit.value;
            r.str_len = expr->as.string_lit.len;
            return r;
        case EXPR_BOOL_LIT:
            r.type = VAL_BOOL;
            r.bool_val = expr->as.bool_lit.value;
            return r;
        case EXPR_VAR_REF: {
            Symbol *sym = sym_find(st, expr->as.var_ref.name);
            if (!sym)
                diag_emit(expr->loc, DIAG_ERROR, "undefined variable '%s'", expr->as.var_ref.name);
            return sym->val;
        }
        case EXPR_BINARY: {
            EvalResult lhs = eval_expr(expr->as.binary.left, st);
            EvalResult rhs = eval_expr(expr->as.binary.right, st);
            return eval_binary(expr->as.binary.op, lhs, rhs, expr->loc);
        }
        case EXPR_MEMBER_ACCESS: {
            EvalResult obj = eval_expr(expr->as.member_access.object, st);
            if (obj.type != VAL_OBJECT || !obj.obj_val)
                diag_emit(expr->loc, DIAG_ERROR, "member access on non-object value");
            const char *fname = expr->as.member_access.field_name;
            for (int i = 0; i < obj.obj_val->field_count; i++) {
                if (strcmp(obj.obj_val->field_names[i], fname) == 0)
                    return obj.obj_val->field_values[i];
            }
            diag_emit(expr->loc, DIAG_ERROR, "no field '%s' on object of class '%s'",
                      fname, obj.obj_val->class_name);
            return r; /* unreachable */
        }
        case EXPR_UNARY: {
            EvalResult operand = eval_expr(expr->as.unary.operand, st);
            if (expr->as.unary.op == UNOP_NEG) {
                if (operand.type == VAL_INT) {
                    r.type = VAL_INT;
                    r.int_val = -operand.int_val;
                } else if (operand.type == VAL_FLOAT) {
                    r.type = VAL_FLOAT;
                    r.float_val = -operand.float_val;
                } else {
                    diag_emit(expr->loc, DIAG_ERROR, "unary '-' requires int or float, got '%s'",
                              value_type_name(operand.type));
                }
            } else if (expr->as.unary.op == UNOP_BIT_NOT) {
                if (operand.type != VAL_INT)
                    diag_emit(expr->loc, DIAG_ERROR, "'~' requires int operand, got '%s'",
                              value_type_name(operand.type));
                r.type = VAL_INT;
                r.int_val = ~operand.int_val;
            }
            return r;
        }
        case EXPR_INDEX: {
            EvalResult obj = eval_expr(expr->as.index_access.object, st);
            EvalResult idx = eval_expr(expr->as.index_access.index, st);
            if (obj.type != VAL_STRING)
                diag_emit(expr->loc, DIAG_ERROR, "indexing requires a string, got '%s'",
                          value_type_name(obj.type));
            if (idx.type != VAL_INT)
                diag_emit(expr->loc, DIAG_ERROR, "index must be an int, got '%s'",
                          value_type_name(idx.type));
            long i = idx.int_val;
            if (i < 0) i += obj.str_len;
            if (i < 0 || i >= obj.str_len)
                diag_emit(expr->loc, DIAG_ERROR, "string index %ld out of range (length %d)",
                          idx.int_val, obj.str_len);
            r.type = VAL_STRING;
            r.str_val = malloc(2);
            r.str_val[0] = obj.str_val[i];
            r.str_val[1] = '\0';
            r.str_len = 1;
            return r;
        }
        case EXPR_SLICE: {
            EvalResult obj = eval_expr(expr->as.slice.object, st);
            EvalResult start = eval_expr(expr->as.slice.start, st);
            EvalResult end = eval_expr(expr->as.slice.end, st);
            if (obj.type != VAL_STRING)
                diag_emit(expr->loc, DIAG_ERROR, "slicing requires a string, got '%s'",
                          value_type_name(obj.type));
            if (start.type != VAL_INT || end.type != VAL_INT)
                diag_emit(expr->loc, DIAG_ERROR, "slice indices must be int");
            long s = start.int_val;
            long e = end.int_val;
            if (s < 0) s += obj.str_len;
            if (e < 0) e += obj.str_len;
            if (s < 0) s = 0;
            if (e > obj.str_len) e = obj.str_len;
            if (s > e) s = e;
            int slen = (int)(e - s);
            r.type = VAL_STRING;
            r.str_val = malloc(slen + 1);
            memcpy(r.str_val, obj.str_val + s, slen);
            r.str_val[slen] = '\0';
            r.str_len = slen;
            return r;
        }
        case EXPR_FN_CALL: {
            /* Evaluate arguments */
            int argc = expr->as.fn_call.arg_count;
            char **av = malloc(argc * sizeof(char *));
            int *al = malloc(argc * sizeof(int));
            ValueType *at = malloc(argc * sizeof(ValueType));
            for (int i = 0; i < argc; i++) {
                EvalResult arg = eval_expr(expr->as.fn_call.args[i], st);
                at[i] = arg.type;
                av[i] = eval_to_string(&arg, &al[i]);
            }

            if (expr->as.fn_call.obj_name) {
                /* Method call in expression context: obj.method(args) */
                ASTNode tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.loc = expr->loc;
                tmp.is_fn_call = 1;
                tmp.obj_name = expr->as.fn_call.obj_name;
                tmp.fn_name = expr->as.fn_call.fn_name;
                tmp.call_arg_count = argc;
                tmp.call_args = av;
                tmp.call_arg_types = at;
                tmp.call_arg_is_var_ref = calloc(argc, sizeof(int));
                tmp.call_arg_names = expr->as.fn_call.arg_names;
                tmp.call_arg_exprs = NULL;
                EvalResult result = evaluate_method_call(&tmp, st, g_ft, g_ct, g_prints, 1);
                free(tmp.call_arg_is_var_ref);
                for (int i = 0; i < argc; i++) free(av[i]);
                free(av); free(al); free(at);
                return result;
            }

            /* Free function call */
            char *rv = NULL; int rl = 0; ValueType rt = VAL_VOID;
            evaluate_fn_call(g_ft, g_ct, st, expr->as.fn_call.fn_name, expr->loc,
                             argc, av, al, at,
                             expr->as.fn_call.arg_names,
                             &rv, &rl, &rt, g_prints);
            for (int i = 0; i < argc; i++) free(av[i]);
            free(av); free(al); free(at);

            if (rt == VAL_VOID)
                diag_emit(expr->loc, DIAG_ERROR, "cannot use void function result in expression");
            r.type = rt;
            switch (rt) {
                case VAL_INT:    r.int_val = atol(rv); break;
                case VAL_FLOAT:  r.float_val = atof(rv); break;
                case VAL_STRING: r.str_val = rv; r.str_len = rl; rv = NULL; break;
                case VAL_BOOL:   r.bool_val = (strcmp(rv, "true") == 0); break;
                default: break;
            }
            free(rv);
            return r;
        }
    }
    return r;
}

/* Convert an EvalResult to a string for output */
static char *eval_to_string(EvalResult *r, int *out_len) {
    char buf[64];
    switch (r->type) {
        case VAL_STRING:
            *out_len = r->str_len;
            {
                char *s = malloc(r->str_len + 1);
                memcpy(s, r->str_val, r->str_len);
                s[r->str_len] = '\0';
                return s;
            }
        case VAL_INT:
            *out_len = snprintf(buf, sizeof(buf), "%ld", r->int_val);
            {
                char *s = malloc(*out_len + 1);
                memcpy(s, buf, *out_len + 1);
                return s;
            }
        case VAL_FLOAT:
            *out_len = snprintf(buf, sizeof(buf), "%g", r->float_val);
            {
                char *s = malloc(*out_len + 1);
                memcpy(s, buf, *out_len + 1);
                return s;
            }
        case VAL_BOOL:
            if (r->bool_val) {
                *out_len = 4;
                char *s = malloc(5);
                memcpy(s, "true", 5);
                return s;
            } else {
                *out_len = 5;
                char *s = malloc(6);
                memcpy(s, "false", 6);
                return s;
            }
        case VAL_OBJECT:
            if (r->obj_val) {
                /* Format: ClassName{field: val, ...} */
                int cap = 256;
                char *s = malloc(cap);
                int pos = snprintf(s, cap, "%s{", r->obj_val->class_name);
                for (int i = 0; i < r->obj_val->field_count; i++) {
                    if (i > 0) { pos += snprintf(s + pos, cap - pos, ", "); }
                    int flen;
                    char *fstr = eval_to_string(&r->obj_val->field_values[i], &flen);
                    int needed = pos + (int)strlen(r->obj_val->field_names[i]) + 2 + flen + 4;
                    if (needed > cap) {
                        cap = needed * 2;
                        s = realloc(s, cap);
                    }
                    pos += snprintf(s + pos, cap - pos, "%s: %s",
                                    r->obj_val->field_names[i], fstr);
                    free(fstr);
                }
                pos += snprintf(s + pos, cap - pos, "}");
                *out_len = pos;
                return s;
            }
            /* fallthrough */
        default:
            *out_len = 0;
            return malloc(1);
    }
}

/* ================================================================
 * Return context for propagating return statements through scopes
 * ================================================================ */

typedef struct {
    int has_return;
    char *return_value;
    int return_len;
    ValueType return_type;
    int has_break;
    int has_continue;
} ReturnCtx;

/* Forward declarations */
static void eval_stmts(ASTNode *stmts, SymTable *st, FnTable *ft, ClassTable *ct, PrintList *prints, ReturnCtx *ret);
static int evaluate_fn_call(FnTable *ft, ClassTable *ct, SymTable *outer_st,
                            const char *fn_name, SourceLoc call_loc,
                            int arg_count,
                            char **arg_values, int *arg_lengths,
                            ValueType *arg_types,
                            char **arg_names,
                            char **ret_value, int *ret_len, ValueType *ret_type,
                            PrintList *prints);

/* ================================================================
 * Helper: resolve fn call args using scope chain
 * ================================================================ */

static void resolve_call_args(ASTNode *n, SymTable *st,
                              char ***out_vals, int **out_lens, ValueType **out_types) {
    *out_vals = malloc(n->call_arg_count * sizeof(char *));
    *out_lens = malloc(n->call_arg_count * sizeof(int));
    *out_types = malloc(n->call_arg_count * sizeof(ValueType));
    for (int i = 0; i < n->call_arg_count; i++) {
        if (n->call_arg_exprs && n->call_arg_exprs[i]) {
            /* Expression-based argument */
            EvalResult rv = eval_expr(n->call_arg_exprs[i], st);
            (*out_types)[i] = rv.type;
            (*out_vals)[i] = eval_to_string(&rv, &(*out_lens)[i]);
        } else if (n->call_arg_is_var_ref[i]) {
            Symbol *sym = sym_find(st, n->call_args[i]);
            if (!sym)
                diag_emit(n->loc, DIAG_ERROR, "undefined variable '%s'", n->call_args[i]);
            (*out_types)[i] = sym->val.type;
            (*out_vals)[i] = eval_to_string(&sym->val, &(*out_lens)[i]);
        } else {
            (*out_vals)[i] = n->call_args[i];
            (*out_lens)[i] = (int)strlen(n->call_args[i]);
            (*out_types)[i] = n->call_arg_types[i];
        }
    }
}

/* ================================================================
 * Helper: evaluate a fn-call RHS and convert to EvalResult
 * ================================================================ */

static EvalResult eval_fn_call_result(ASTNode *n, SymTable *st, FnTable *ft,
                                      ClassTable *ct, PrintList *prints, int require_value) {
    char **av; int *al; ValueType *at;
    resolve_call_args(n, st, &av, &al, &at);
    char *rv = NULL; int rl = 0; ValueType rt = VAL_VOID;
    evaluate_fn_call(ft, ct, st, n->fn_name, n->loc,
                     n->call_arg_count, av, al, at,
                     n->call_arg_names, &rv, &rl, &rt, prints);
    free(av); free(al); free(at);

    EvalResult val;
    memset(&val, 0, sizeof(val));
    if (require_value && rt == VAL_VOID)
        diag_emit(n->loc, DIAG_ERROR, "cannot use void function result");
    val.type = rt;
    switch (rt) {
        case VAL_INT:    val.int_val = atol(rv); break;
        case VAL_FLOAT:  val.float_val = atof(rv); break;
        case VAL_STRING: val.str_val = rv; val.str_len = rl; break;
        case VAL_BOOL:   val.bool_val = (strcmp(rv, "true") == 0); break;
        default: break;
    }
    if (rt != VAL_STRING) free(rv);
    return val;
}

/* ================================================================
 * eval_new_expr — construct a new object instance
 * ================================================================ */

static EvalResult eval_new_expr(ASTNode *n, SymTable *st, ClassTable *ct) {
    ClassDef *cls = class_table_find(ct, n->fn_name);
    if (!cls)
        diag_emit(n->loc, DIAG_ERROR, "undefined class '%s'", n->fn_name);

    /* Resolve arguments */
    int arg_count = n->call_arg_count;
    EvalResult *arg_vals = malloc(arg_count * sizeof(EvalResult));
    for (int i = 0; i < arg_count; i++) {
        if (n->call_arg_exprs && n->call_arg_exprs[i]) {
            arg_vals[i] = eval_expr(n->call_arg_exprs[i], st);
        } else if (n->call_arg_is_var_ref[i]) {
            Symbol *sym = sym_find(st, n->call_args[i]);
            if (!sym)
                diag_emit(n->loc, DIAG_ERROR, "undefined variable '%s'", n->call_args[i]);
            arg_vals[i] = sym->val;
        } else {
            memset(&arg_vals[i], 0, sizeof(EvalResult));
            arg_vals[i].type = n->call_arg_types[i];
            switch (n->call_arg_types[i]) {
                case VAL_INT:    arg_vals[i].int_val = atol(n->call_args[i]); break;
                case VAL_FLOAT:  arg_vals[i].float_val = atof(n->call_args[i]); break;
                case VAL_STRING: arg_vals[i].str_val = n->call_args[i];
                                 arg_vals[i].str_len = (int)strlen(n->call_args[i]); break;
                case VAL_BOOL:   arg_vals[i].bool_val = (strcmp(n->call_args[i], "true") == 0); break;
                default: break;
            }
        }
    }

    /* Match args to class fields (same logic as fn call: positional then named) */
    ObjData *obj = malloc(sizeof(ObjData));
    obj->class_name = cls->name;
    obj->field_count = cls->field_count;
    obj->field_names = cls->field_names;
    obj->field_values = malloc(cls->field_count * sizeof(EvalResult));
    int *filled = calloc(cls->field_count, sizeof(int));

    int has_named = 0;
    if (n->call_arg_names) {
        for (int i = 0; i < arg_count; i++)
            if (n->call_arg_names[i]) { has_named = 1; break; }
    }

    if (has_named) {
        int pos_idx = 0;
        for (int i = 0; i < arg_count; i++) {
            if (n->call_arg_names && n->call_arg_names[i]) continue;
            if (pos_idx >= cls->field_count)
                diag_emit(n->loc, DIAG_ERROR, "too many positional arguments for class '%s'", cls->name);
            obj->field_values[pos_idx] = arg_vals[i];
            filled[pos_idx] = 1;
            pos_idx++;
        }
        for (int i = 0; i < arg_count; i++) {
            if (!n->call_arg_names || !n->call_arg_names[i]) continue;
            int found = 0;
            for (int f = 0; f < cls->field_count; f++) {
                if (strcmp(n->call_arg_names[i], cls->field_names[f]) == 0) {
                    if (filled[f])
                        diag_emit(n->loc, DIAG_ERROR, "duplicate argument for field '%s' in class '%s'",
                                  n->call_arg_names[i], cls->name);
                    obj->field_values[f] = arg_vals[i];
                    filled[f] = 1;
                    found = 1;
                    break;
                }
            }
            if (!found)
                diag_emit(n->loc, DIAG_ERROR, "unknown field '%s' in class '%s'",
                          n->call_arg_names[i], cls->name);
        }
    } else {
        if (arg_count != cls->field_count)
            diag_emit(n->loc, DIAG_ERROR, "class '%s' has %d field(s), got %d argument(s)",
                      cls->name, cls->field_count, arg_count);
        for (int i = 0; i < arg_count; i++) {
            obj->field_values[i] = arg_vals[i];
            filled[i] = 1;
        }
    }

    /* Check all fields filled and type-check */
    for (int i = 0; i < cls->field_count; i++) {
        if (!filled[i])
            diag_emit(n->loc, DIAG_ERROR, "missing value for field '%s' in class '%s'",
                      cls->field_names[i], cls->name);
        if (obj->field_values[i].type != cls->field_types[i])
            diag_emit(n->loc, DIAG_ERROR, "field '%s' expects '%s', got '%s'",
                      cls->field_names[i], value_type_name(cls->field_types[i]),
                      value_type_name(obj->field_values[i].type));
    }

    free(filled);
    free(arg_vals);

    EvalResult val;
    memset(&val, 0, sizeof(val));
    val.type = VAL_OBJECT;
    val.obj_val = obj;
    return val;
}

/* ================================================================
 * evaluate_method_call — call a method on an object
 * ================================================================ */

static EvalResult evaluate_method_call(ASTNode *n, SymTable *st, FnTable *ft,
                                       ClassTable *ct, PrintList *prints,
                                       int require_value) {
    Symbol *obj_sym = sym_find(st, n->obj_name);
    if (!obj_sym)
        diag_emit(n->loc, DIAG_ERROR, "undefined variable '%s'", n->obj_name);
    if (obj_sym->val.type != VAL_OBJECT || !obj_sym->val.obj_val)
        diag_emit(n->loc, DIAG_ERROR, "'%s' is not an object", n->obj_name);

    ObjData *obj = obj_sym->val.obj_val;
    const char *method_name = n->fn_name;

    /* Walk inheritance chain to find method */
    ASTNode *method_decl = NULL;
    ClassDef *cls = class_table_find(ct, obj->class_name);
    while (cls) {
        for (ASTNode *m = cls->methods; m; m = m->next) {
            if (strcmp(m->fn_name, method_name) == 0) {
                method_decl = m;
                break;
            }
        }
        if (method_decl) break;
        cls = cls->parent_name ? class_table_find(ct, cls->parent_name) : NULL;
    }
    if (!method_decl)
        diag_emit(n->loc, DIAG_ERROR, "no method '%s' on class '%s'",
                  method_name, obj->class_name);

    /* Build local scope: object fields + method params */
    SymTable local_st;
    sym_table_init(&local_st);
    local_st.parent = st;

    /* Add object fields as local variables */
    for (int i = 0; i < obj->field_count; i++) {
        sym_add(&local_st, obj->field_names[i], obj->field_values[i], 0, n->loc);
    }

    /* Resolve and add method parameters */
    int arg_count = n->call_arg_count;
    for (int i = 0; i < arg_count; i++) {
        EvalResult pval;
        memset(&pval, 0, sizeof(pval));
        if (n->call_arg_exprs && n->call_arg_exprs[i]) {
            pval = eval_expr(n->call_arg_exprs[i], st);
        } else if (n->call_arg_is_var_ref[i]) {
            Symbol *asym = sym_find(st, n->call_args[i]);
            if (!asym)
                diag_emit(n->loc, DIAG_ERROR, "undefined variable '%s'", n->call_args[i]);
            pval = asym->val;
        } else {
            pval.type = n->call_arg_types[i];
            switch (n->call_arg_types[i]) {
                case VAL_INT:    pval.int_val = atol(n->call_args[i]); break;
                case VAL_FLOAT:  pval.float_val = atof(n->call_args[i]); break;
                case VAL_STRING: pval.str_val = n->call_args[i]; pval.str_len = (int)strlen(n->call_args[i]); break;
                case VAL_BOOL:   pval.bool_val = (strcmp(n->call_args[i], "true") == 0); break;
                default: break;
            }
        }
        if (i < method_decl->param_count) {
            if (pval.type != method_decl->params[i].type)
                diag_emit(n->loc, DIAG_ERROR, "method '%s' parameter '%s' expects '%s', got '%s'",
                          method_name, method_decl->params[i].name,
                          value_type_name(method_decl->params[i].type),
                          value_type_name(pval.type));
            sym_add(&local_st, method_decl->params[i].name, pval, 1, n->loc);
        }
    }

    if (arg_count < method_decl->param_count) {
        /* Fill defaults */
        for (int i = arg_count; i < method_decl->param_count; i++) {
            if (!method_decl->params[i].has_default)
                diag_emit(n->loc, DIAG_ERROR, "missing argument for parameter '%s' in method '%s'",
                          method_decl->params[i].name, method_name);
            EvalResult pval;
            memset(&pval, 0, sizeof(pval));
            pval.type = method_decl->params[i].type;
            switch (pval.type) {
                case VAL_INT:    pval.int_val = atol(method_decl->params[i].default_value); break;
                case VAL_FLOAT:  pval.float_val = atof(method_decl->params[i].default_value); break;
                case VAL_STRING: pval.str_val = method_decl->params[i].default_value;
                                 pval.str_len = method_decl->params[i].default_value_len; break;
                case VAL_BOOL:   pval.bool_val = (strcmp(method_decl->params[i].default_value, "true") == 0); break;
                default: break;
            }
            sym_add(&local_st, method_decl->params[i].name, pval, 1, n->loc);
        }
    }

    ReturnCtx ret_ctx;
    memset(&ret_ctx, 0, sizeof(ret_ctx));
    ret_ctx.return_type = VAL_VOID;

    eval_stmts(method_decl->body, &local_st, ft, ct, prints, &ret_ctx);

    /* Propagate field mutations back to the object */
    for (int i = 0; i < obj->field_count; i++) {
        int idx = sym_lookup(&local_st, obj->field_names[i]);
        if (idx >= 0)
            obj->field_values[i] = local_st.syms[idx].val;
    }

    sym_table_free(&local_st);

    if (method_decl->has_return_type && !ret_ctx.has_return)
        diag_emit(n->loc, DIAG_ERROR, "method '%s' must return a value", method_name);
    if (ret_ctx.has_return && method_decl->has_return_type &&
        ret_ctx.return_type != method_decl->return_type)
        diag_emit(n->loc, DIAG_ERROR, "method '%s' returns '%s', expected '%s'",
                  method_name, value_type_name(ret_ctx.return_type),
                  value_type_name(method_decl->return_type));

    EvalResult result;
    memset(&result, 0, sizeof(result));
    if (require_value && !ret_ctx.has_return)
        diag_emit(n->loc, DIAG_ERROR, "cannot use void method result");
    if (ret_ctx.has_return) {
        result.type = ret_ctx.return_type;
        switch (ret_ctx.return_type) {
            case VAL_INT:    result.int_val = atol(ret_ctx.return_value); break;
            case VAL_FLOAT:  result.float_val = atof(ret_ctx.return_value); break;
            case VAL_STRING: result.str_val = ret_ctx.return_value;
                             result.str_len = ret_ctx.return_len; break;
            case VAL_BOOL:   result.bool_val = (strcmp(ret_ctx.return_value, "true") == 0); break;
            default: break;
        }
        if (ret_ctx.return_type != VAL_STRING) free(ret_ctx.return_value);
    }
    return result;
}

/* ================================================================
 * eval_stmts — unified statement evaluator with block scoping
 * ================================================================ */

static void eval_stmts(ASTNode *stmts, SymTable *st, FnTable *ft, ClassTable *ct, PrintList *prints, ReturnCtx *ret) {
    for (ASTNode *n = stmts; n; n = n->next) {
        if (ret && (ret->has_return || ret->has_break || ret->has_continue)) return;

        if (n->type == NODE_FN_DECL) continue;
        if (n->type == NODE_CLASS_DECL) continue; /* collected in first pass */
        if (n->type == NODE_IMPORT) continue;     /* processed in pass 0 */

        if (n->type == NODE_BREAK) {
            if (ret) ret->has_break = 1;
            return;
        }

        if (n->type == NODE_CONTINUE) {
            if (ret) ret->has_continue = 1;
            return;
        }

        if (n->type == NODE_RETURN) {
            if (!ret) {
                diag_emit(n->loc, DIAG_ERROR, "return statement outside of function");
                return;
            }
            if (n->is_fn_call && n->obj_name) {
                /* return obj.method(args); */
                EvalResult rv = evaluate_method_call(n, st, ft, ct, prints, 1);
                ret->return_value = eval_to_string(&rv, &ret->return_len);
                ret->return_type = rv.type;
            } else if (n->is_fn_call) {
                char **av; int *al; ValueType *at;
                resolve_call_args(n, st, &av, &al, &at);
                char *rv = NULL; int rl = 0; ValueType rt = VAL_VOID;
                evaluate_fn_call(ft, ct, st, n->fn_name, n->loc,
                                 n->call_arg_count, av, al, at,
                                 NULL, &rv, &rl, &rt, prints);
                free(av); free(al); free(at);
                ret->return_value = rv;
                ret->return_len = rl;
                ret->return_type = rt;
            } else if (n->expr) {
                EvalResult rv = eval_expr(n->expr, st);
                ret->return_value = eval_to_string(&rv, &ret->return_len);
                ret->return_type = rv.type;
            }
            ret->has_return = 1;
            return;
        }

        if (n->type == NODE_VAR_DECL) {
            EvalResult val;
            if (n->is_new_expr) {
                val = eval_new_expr(n, st, ct);
            } else if (n->is_fn_call && n->obj_name) {
                val = evaluate_method_call(n, st, ft, ct, prints, 1);
            } else if (n->is_fn_call) {
                val = eval_fn_call_result(n, st, ft, ct, prints, 1);
            } else {
                val = eval_expr(n->expr, st);
            }
            sym_add(st, n->var_name, val, n->is_const, n->loc);
        } else if (n->type == NODE_ASSIGN) {
            if (n->field_name) {
                /* Field assignment: obj.field = value; */
                Symbol *sym = sym_find(st, n->var_name);
                if (!sym)
                    diag_emit(n->loc, DIAG_ERROR, "undefined variable '%s'", n->var_name);
                if (sym->is_const)
                    diag_emit(n->loc, DIAG_ERROR, "cannot mutate fields of const variable '%s'", n->var_name);
                if (sym->val.type != VAL_OBJECT || !sym->val.obj_val)
                    diag_emit(n->loc, DIAG_ERROR, "'%s' is not an object", n->var_name);
                ObjData *obj = sym->val.obj_val;
                int found = 0;
                for (int i = 0; i < obj->field_count; i++) {
                    if (strcmp(obj->field_names[i], n->field_name) == 0) {
                        EvalResult val;
                        if (n->is_fn_call && n->obj_name) {
                            val = evaluate_method_call(n, st, ft, ct, prints, 1);
                        } else if (n->is_fn_call) {
                            val = eval_fn_call_result(n, st, ft, ct, prints, 1);
                        } else {
                            val = eval_expr(n->expr, st);
                        }
                        if (obj->field_values[i].type != val.type)
                            diag_emit(n->loc, DIAG_ERROR,
                                      "type mismatch: field '%s' has type '%s', cannot assign '%s'",
                                      n->field_name, value_type_name(obj->field_values[i].type),
                                      value_type_name(val.type));
                        obj->field_values[i] = val;
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    diag_emit(n->loc, DIAG_ERROR, "no field '%s' on object of class '%s'",
                              n->field_name, obj->class_name);
                sym->mutated = 1;
            } else {
                /* Regular assignment */
                EvalResult val;
                if (n->is_fn_call && n->obj_name) {
                    val = evaluate_method_call(n, st, ft, ct, prints, 1);
                } else if (n->is_fn_call) {
                    val = eval_fn_call_result(n, st, ft, ct, prints, 1);
                } else {
                    val = eval_expr(n->expr, st);
                }
                Symbol *sym = sym_find(st, n->var_name);
                if (!sym)
                    diag_emit(n->loc, DIAG_ERROR, "undefined variable '%s'", n->var_name);
                if (sym->is_const)
                    diag_emit(n->loc, DIAG_ERROR, "cannot reassign const variable '%s'", n->var_name);
                if (sym->val.type != val.type)
                    diag_emit(n->loc, DIAG_ERROR, "type mismatch: variable '%s' has type '%s', cannot assign '%s'",
                              n->var_name, value_type_name(sym->val.type), value_type_name(val.type));
                sym->val = val;
                sym->mutated = 1;
            }
        } else if (n->type == NODE_PRINT) {
            EvalResult val;
            if (n->is_fn_call && n->obj_name) {
                val = evaluate_method_call(n, st, ft, ct, prints, 1);
            } else if (n->is_fn_call) {
                val = eval_fn_call_result(n, st, ft, ct, prints, 1);
            } else {
                val = eval_expr(n->expr, st);
            }
            int slen;
            char *s = eval_to_string(&val, &slen);
            print_list_add(prints, s, slen);
            if (n->print_newline) {
                print_list_add(prints, "\n", 1);
            }
        } else if (n->type == NODE_FN_CALL) {
            if (n->obj_name) {
                /* Standalone method call: obj.method(args); */
                evaluate_method_call(n, st, ft, ct, prints, 0);
            } else {
                char **av; int *al; ValueType *at;
                resolve_call_args(n, st, &av, &al, &at);
                char *rv = NULL; int rl = 0; ValueType rt = VAL_VOID;
                evaluate_fn_call(ft, ct, st, n->fn_name, n->loc,
                                 n->call_arg_count, av, al, at,
                                 n->call_arg_names, &rv, &rl, &rt, prints);
                free(av); free(al); free(at);
                free(rv);
            }
        } else if (n->type == NODE_BLOCK) {
            SymTable child;
            sym_table_init(&child);
            child.parent = st;
            eval_stmts(n->body, &child, ft, ct, prints, ret);
            for (int j = 0; j < child.count; j++) {
                if (!child.syms[j].is_const && !child.syms[j].mutated)
                    diag_emit(child.syms[j].loc, DIAG_WARNING, "variable '%s' is never mutated, consider using 'const'", child.syms[j].name);
            }
            sym_table_free(&child);
        } else if (n->type == NODE_FOR_LOOP) {
            SymTable loop_st;
            sym_table_init(&loop_st);
            loop_st.parent = st;
            eval_stmts(n->for_init, &loop_st, ft, ct, prints, NULL);
            for (int iter = 0; ; iter++) {
                if (iter >= 10000)
                    diag_emit(n->loc, DIAG_ERROR, "for loop exceeded 10000 iterations (possible infinite loop)");
                EvalResult cond = eval_expr(n->for_cond, &loop_st);
                if (cond.type != VAL_BOOL)
                    diag_emit(n->loc, DIAG_ERROR, "for loop condition must be a bool");
                if (!cond.bool_val) break;
                ReturnCtx loop_ret;
                memset(&loop_ret, 0, sizeof(loop_ret));
                SymTable body_st;
                sym_table_init(&body_st);
                body_st.parent = &loop_st;
                eval_stmts(n->body, &body_st, ft, ct, prints, &loop_ret);
                sym_table_free(&body_st);
                if (loop_ret.has_return) {
                    /* Propagate return up */
                    if (ret) {
                        ret->has_return = 1;
                        ret->return_value = loop_ret.return_value;
                        ret->return_len = loop_ret.return_len;
                        ret->return_type = loop_ret.return_type;
                    }
                    break;
                }
                if (loop_ret.has_break) break;
                /* continue: skip to update */
                eval_stmts(n->for_update, &loop_st, ft, ct, prints, NULL);
            }
            sym_table_free(&loop_st);
        } else if (n->type == NODE_IF_STMT) {
            ASTNode *branch = n;
            ASTNode *taken_body = NULL;
            while (branch && branch->type == NODE_IF_STMT && branch->if_cond) {
                EvalResult cond = eval_expr(branch->if_cond, st);
                if (cond.type != VAL_BOOL)
                    diag_emit(branch->loc, DIAG_ERROR, "if condition must be a bool, got '%s'", value_type_name(cond.type));
                if (cond.bool_val) {
                    taken_body = branch->if_body;
                    break;
                }
                branch = branch->else_body;
            }
            if (!taken_body && branch && branch->type != NODE_IF_STMT)
                taken_body = branch;

            if (taken_body) {
                SymTable if_st;
                sym_table_init(&if_st);
                if_st.parent = st;
                eval_stmts(taken_body, &if_st, ft, ct, prints, ret);
                for (int j = 0; j < if_st.count; j++) {
                    if (!if_st.syms[j].is_const && !if_st.syms[j].mutated)
                        diag_emit(if_st.syms[j].loc, DIAG_WARNING, "variable '%s' is never mutated, consider using 'const'", if_st.syms[j].name);
                }
                sym_table_free(&if_st);
            }
        } else if (n->type == NODE_MATCH_STMT) {
            EvalResult scrutinee = eval_expr(n->match_expr, st);
            int matched = 0;
            for (int a = 0; a < n->match_arm_count; a++) {
                MatchArm *arm = &n->match_arms[a];
                if (arm->is_wildcard) {
                    matched = 1;
                } else {
                    EvalResult pat = eval_expr(arm->pattern, st);
                    EvalResult cmp = eval_binary(BINOP_EQ, scrutinee, pat, n->loc);
                    if (cmp.bool_val)
                        matched = 1;
                }
                if (matched) {
                    SymTable match_st;
                    sym_table_init(&match_st);
                    match_st.parent = st;
                    eval_stmts(arm->body, &match_st, ft, ct, prints, ret);
                    for (int j = 0; j < match_st.count; j++) {
                        if (!match_st.syms[j].is_const && !match_st.syms[j].mutated)
                            diag_emit(match_st.syms[j].loc, DIAG_WARNING, "variable '%s' is never mutated, consider using 'const'", match_st.syms[j].name);
                    }
                    sym_table_free(&match_st);
                    break;
                }
            }
        }
    }
}

/* ================================================================
 * Built-in string functions
 * ================================================================ */

static int eval_builtin_string_fn(const char *fn_name, SourceLoc call_loc,
                                  int arg_count, char **arg_values,
                                  int *arg_lengths, ValueType *arg_types,
                                  char **ret_value, int *ret_len, ValueType *ret_type) {
    /* len(s) -> int */
    if (strcmp(fn_name, "len") == 0) {
        if (arg_count != 1) diag_emit(call_loc, DIAG_ERROR, "len() expects 1 argument, got %d", arg_count);
        if (arg_types[0] != VAL_STRING) diag_emit(call_loc, DIAG_ERROR, "len() expects a string argument");
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d", arg_lengths[0]);
        *ret_value = malloc(n + 1);
        memcpy(*ret_value, buf, n + 1);
        *ret_len = n;
        *ret_type = VAL_INT;
        return 1;
    }
    /* trim(s) -> string */
    if (strcmp(fn_name, "trim") == 0) {
        if (arg_count != 1) diag_emit(call_loc, DIAG_ERROR, "trim() expects 1 argument");
        if (arg_types[0] != VAL_STRING) diag_emit(call_loc, DIAG_ERROR, "trim() expects a string argument");
        const char *s = arg_values[0];
        int slen = arg_lengths[0];
        int start = 0, end = slen;
        while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) start++;
        while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\n' || s[end-1] == '\r')) end--;
        int rlen = end - start;
        *ret_value = malloc(rlen + 1);
        memcpy(*ret_value, s + start, rlen);
        (*ret_value)[rlen] = '\0';
        *ret_len = rlen;
        *ret_type = VAL_STRING;
        return 1;
    }
    /* contains(s, sub) -> bool */
    if (strcmp(fn_name, "contains") == 0) {
        if (arg_count != 2) diag_emit(call_loc, DIAG_ERROR, "contains() expects 2 arguments");
        if (arg_types[0] != VAL_STRING || arg_types[1] != VAL_STRING)
            diag_emit(call_loc, DIAG_ERROR, "contains() expects string arguments");
        int found = strstr(arg_values[0], arg_values[1]) != NULL;
        *ret_value = malloc(6);
        strcpy(*ret_value, found ? "true" : "false");
        *ret_len = found ? 4 : 5;
        *ret_type = VAL_BOOL;
        return 1;
    }
    /* replace(s, old, new) -> string */
    if (strcmp(fn_name, "replace") == 0) {
        if (arg_count != 3) diag_emit(call_loc, DIAG_ERROR, "replace() expects 3 arguments");
        if (arg_types[0] != VAL_STRING || arg_types[1] != VAL_STRING || arg_types[2] != VAL_STRING)
            diag_emit(call_loc, DIAG_ERROR, "replace() expects string arguments");
        const char *s = arg_values[0];
        const char *old = arg_values[1];
        const char *new_str = arg_values[2];
        int old_len = arg_lengths[1];
        int new_len = arg_lengths[2];
        if (old_len == 0) {
            /* Replace empty string: return original */
            *ret_value = malloc(arg_lengths[0] + 1);
            memcpy(*ret_value, s, arg_lengths[0] + 1);
            *ret_len = arg_lengths[0];
            *ret_type = VAL_STRING;
            return 1;
        }
        int cap = arg_lengths[0] * 2 + 1;
        char *result = malloc(cap);
        int rpos = 0;
        const char *p = s;
        while (*p) {
            const char *found = strstr(p, old);
            if (!found) {
                int remain = (int)strlen(p);
                while (rpos + remain + 1 > cap) { cap *= 2; result = realloc(result, cap); }
                memcpy(result + rpos, p, remain);
                rpos += remain;
                break;
            }
            int seg = (int)(found - p);
            while (rpos + seg + new_len + 1 > cap) { cap *= 2; result = realloc(result, cap); }
            memcpy(result + rpos, p, seg);
            rpos += seg;
            memcpy(result + rpos, new_str, new_len);
            rpos += new_len;
            p = found + old_len;
        }
        result[rpos] = '\0';
        *ret_value = result;
        *ret_len = rpos;
        *ret_type = VAL_STRING;
        return 1;
    }
    /* to_upper(s) -> string */
    if (strcmp(fn_name, "to_upper") == 0) {
        if (arg_count != 1) diag_emit(call_loc, DIAG_ERROR, "to_upper() expects 1 argument");
        if (arg_types[0] != VAL_STRING) diag_emit(call_loc, DIAG_ERROR, "to_upper() expects a string argument");
        int slen = arg_lengths[0];
        char *r = malloc(slen + 1);
        for (int i = 0; i < slen; i++) {
            char c = arg_values[0][i];
            r[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
        r[slen] = '\0';
        *ret_value = r;
        *ret_len = slen;
        *ret_type = VAL_STRING;
        return 1;
    }
    /* to_lower(s) -> string */
    if (strcmp(fn_name, "to_lower") == 0) {
        if (arg_count != 1) diag_emit(call_loc, DIAG_ERROR, "to_lower() expects 1 argument");
        if (arg_types[0] != VAL_STRING) diag_emit(call_loc, DIAG_ERROR, "to_lower() expects a string argument");
        int slen = arg_lengths[0];
        char *r = malloc(slen + 1);
        for (int i = 0; i < slen; i++) {
            char c = arg_values[0][i];
            r[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }
        r[slen] = '\0';
        *ret_value = r;
        *ret_len = slen;
        *ret_type = VAL_STRING;
        return 1;
    }
    /* starts_with(s, prefix) -> bool */
    if (strcmp(fn_name, "starts_with") == 0) {
        if (arg_count != 2) diag_emit(call_loc, DIAG_ERROR, "starts_with() expects 2 arguments");
        if (arg_types[0] != VAL_STRING || arg_types[1] != VAL_STRING)
            diag_emit(call_loc, DIAG_ERROR, "starts_with() expects string arguments");
        int slen = arg_lengths[0], plen = arg_lengths[1];
        int match = (plen <= slen && memcmp(arg_values[0], arg_values[1], plen) == 0);
        *ret_value = malloc(6);
        strcpy(*ret_value, match ? "true" : "false");
        *ret_len = match ? 4 : 5;
        *ret_type = VAL_BOOL;
        return 1;
    }
    /* ends_with(s, suffix) -> bool */
    if (strcmp(fn_name, "ends_with") == 0) {
        if (arg_count != 2) diag_emit(call_loc, DIAG_ERROR, "ends_with() expects 2 arguments");
        if (arg_types[0] != VAL_STRING || arg_types[1] != VAL_STRING)
            diag_emit(call_loc, DIAG_ERROR, "ends_with() expects string arguments");
        int slen = arg_lengths[0], suflen = arg_lengths[1];
        int match = (suflen <= slen && memcmp(arg_values[0] + slen - suflen, arg_values[1], suflen) == 0);
        *ret_value = malloc(6);
        strcpy(*ret_value, match ? "true" : "false");
        *ret_len = match ? 4 : 5;
        *ret_type = VAL_BOOL;
        return 1;
    }
    /* index_of(s, sub) -> int */
    if (strcmp(fn_name, "index_of") == 0) {
        if (arg_count != 2) diag_emit(call_loc, DIAG_ERROR, "index_of() expects 2 arguments");
        if (arg_types[0] != VAL_STRING || arg_types[1] != VAL_STRING)
            diag_emit(call_loc, DIAG_ERROR, "index_of() expects string arguments");
        const char *found = strstr(arg_values[0], arg_values[1]);
        long idx = found ? (long)(found - arg_values[0]) : -1;
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%ld", idx);
        *ret_value = malloc(n + 1);
        memcpy(*ret_value, buf, n + 1);
        *ret_len = n;
        *ret_type = VAL_INT;
        return 1;
    }
    /* char_at(s, i) -> string */
    if (strcmp(fn_name, "char_at") == 0) {
        if (arg_count != 2) diag_emit(call_loc, DIAG_ERROR, "char_at() expects 2 arguments");
        if (arg_types[0] != VAL_STRING) diag_emit(call_loc, DIAG_ERROR, "char_at() first argument must be a string");
        if (arg_types[1] != VAL_INT) diag_emit(call_loc, DIAG_ERROR, "char_at() second argument must be an int");
        long idx = atol(arg_values[1]);
        int slen = arg_lengths[0];
        if (idx < 0) idx += slen;
        if (idx < 0 || idx >= slen)
            diag_emit(call_loc, DIAG_ERROR, "char_at() index %ld out of range (length %d)", idx, slen);
        *ret_value = malloc(2);
        (*ret_value)[0] = arg_values[0][idx];
        (*ret_value)[1] = '\0';
        *ret_len = 1;
        *ret_type = VAL_STRING;
        return 1;
    }
    /* substr(s, start, end) -> string */
    if (strcmp(fn_name, "substr") == 0) {
        if (arg_count != 3) diag_emit(call_loc, DIAG_ERROR, "substr() expects 3 arguments");
        if (arg_types[0] != VAL_STRING) diag_emit(call_loc, DIAG_ERROR, "substr() first argument must be a string");
        if (arg_types[1] != VAL_INT || arg_types[2] != VAL_INT)
            diag_emit(call_loc, DIAG_ERROR, "substr() start and end must be int");
        long s = atol(arg_values[1]);
        long e = atol(arg_values[2]);
        int slen = arg_lengths[0];
        if (s < 0) s += slen;
        if (e < 0) e += slen;
        if (s < 0) s = 0;
        if (e > slen) e = slen;
        if (s > e) s = e;
        int rlen = (int)(e - s);
        *ret_value = malloc(rlen + 1);
        memcpy(*ret_value, arg_values[0] + s, rlen);
        (*ret_value)[rlen] = '\0';
        *ret_len = rlen;
        *ret_type = VAL_STRING;
        return 1;
    }
    return 0; /* not a built-in */
}

/* ================================================================
 * Compile-time function evaluation
 * ================================================================ */

static int evaluate_fn_call(FnTable *ft, ClassTable *ct, SymTable *outer_st,
                            const char *fn_name, SourceLoc call_loc,
                            int arg_count,
                            char **arg_values, int *arg_lengths,
                            ValueType *arg_types,
                            char **arg_names,
                            char **ret_value, int *ret_len, ValueType *ret_type,
                            PrintList *prints) {
    /* User-defined functions take priority over stdlib */
    FnEntry *fn = fn_table_find(ft, fn_name);
    if (!fn) {
        /* Fall back to stdlib built-ins if imported */
        if (stdlib_fn_is_imported(fn_name) &&
            eval_builtin_string_fn(fn_name, call_loc, arg_count, arg_values,
                                   arg_lengths, arg_types, ret_value, ret_len, ret_type))
            return 0;
        diag_emit(call_loc, DIAG_ERROR, "undefined function '%s'", fn_name);
    }

    ASTNode *decl = fn->decl;

    int required_count = 0;
    for (int i = 0; i < decl->param_count; i++) {
        if (!decl->params[i].has_default)
            required_count++;
    }

    if (arg_count > decl->param_count)
        diag_emit(call_loc, DIAG_ERROR, "function '%s' expects at most %d argument(s), got %d",
                  fn_name, decl->param_count, arg_count);

    char **final_values = malloc(decl->param_count * sizeof(char *));
    int *final_lengths = malloc(decl->param_count * sizeof(int));
    ValueType *final_types = malloc(decl->param_count * sizeof(ValueType));
    int *final_filled = calloc(decl->param_count, sizeof(int));

    int has_named = 0;
    if (arg_names) {
        for (int i = 0; i < arg_count; i++) {
            if (arg_names[i]) { has_named = 1; break; }
        }
    }

    if (has_named) {
        int pos_idx = 0;
        for (int i = 0; i < arg_count; i++) {
            if (arg_names && arg_names[i]) continue;
            if (pos_idx >= decl->param_count)
                diag_emit(call_loc, DIAG_ERROR, "too many positional arguments for function '%s'", fn_name);
            final_values[pos_idx] = arg_values[i];
            final_lengths[pos_idx] = arg_lengths[i];
            final_types[pos_idx] = arg_types[i];
            final_filled[pos_idx] = 1;
            pos_idx++;
        }
        for (int i = 0; i < arg_count; i++) {
            if (!arg_names || !arg_names[i]) continue;
            int found = 0;
            for (int p = 0; p < decl->param_count; p++) {
                if (strcmp(arg_names[i], decl->params[p].name) == 0) {
                    if (final_filled[p])
                        diag_emit(call_loc, DIAG_ERROR, "duplicate argument for parameter '%s' in function '%s'",
                                  arg_names[i], fn_name);
                    final_values[p] = arg_values[i];
                    final_lengths[p] = arg_lengths[i];
                    final_types[p] = arg_types[i];
                    final_filled[p] = 1;
                    found = 1;
                    break;
                }
            }
            if (!found)
                diag_emit(call_loc, DIAG_ERROR, "unknown parameter '%s' in function '%s'",
                          arg_names[i], fn_name);
        }
    } else {
        for (int i = 0; i < arg_count; i++) {
            final_values[i] = arg_values[i];
            final_lengths[i] = arg_lengths[i];
            final_types[i] = arg_types[i];
            final_filled[i] = 1;
        }
    }

    for (int i = 0; i < decl->param_count; i++) {
        if (!final_filled[i]) {
            if (!decl->params[i].has_default)
                diag_emit(call_loc, DIAG_ERROR, "missing argument for required parameter '%s' in function '%s'",
                          decl->params[i].name, fn_name);
            final_values[i] = decl->params[i].default_value;
            final_lengths[i] = decl->params[i].default_value_len;
            final_types[i] = decl->params[i].type;
            final_filled[i] = 1;
        }
    }

    for (int i = 0; i < decl->param_count; i++) {
        if (final_types[i] != decl->params[i].type)
            diag_emit(call_loc, DIAG_ERROR, "function '%s' parameter '%s' expects '%s', got '%s'",
                      fn_name, decl->params[i].name,
                      value_type_name(decl->params[i].type),
                      value_type_name(final_types[i]));
    }

    arg_count = decl->param_count;
    arg_values = final_values;
    arg_lengths = final_lengths;
    arg_types = final_types;

    /* Recursion depth limit */
    if (ft->eval_count >= 1000)
        diag_emit(call_loc, DIAG_ERROR, "recursion depth limit exceeded (1000) in function '%s'", fn_name);
    if (ft->eval_count == ft->eval_cap) {
        ft->eval_cap *= 2;
        ft->evaluating = realloc(ft->evaluating, ft->eval_cap * sizeof(char *));
    }
    ft->evaluating[ft->eval_count++] = (char *)fn_name;

    /* Build local symbol table with parent chain to outer scope */
    SymTable local_st;
    sym_table_init(&local_st);
    local_st.parent = outer_st;

    for (int i = 0; i < arg_count; i++) {
        EvalResult pval;
        memset(&pval, 0, sizeof(pval));
        pval.type = arg_types[i];
        switch (arg_types[i]) {
            case VAL_INT:    pval.int_val = atol(arg_values[i]); break;
            case VAL_FLOAT:  pval.float_val = atof(arg_values[i]); break;
            case VAL_STRING: pval.str_val = arg_values[i]; pval.str_len = arg_lengths[i]; break;
            case VAL_BOOL:   pval.bool_val = (strcmp(arg_values[i], "true") == 0); break;
            default: break;
        }
        sym_add(&local_st, decl->params[i].name, pval, 1, decl->loc);
    }

    ReturnCtx ret_ctx;
    memset(&ret_ctx, 0, sizeof(ret_ctx));
    ret_ctx.return_type = VAL_VOID;

    eval_stmts(decl->body, &local_st, ft, ct, prints, &ret_ctx);

    ft->eval_count--;

    if (decl->has_return_type && !ret_ctx.has_return)
        diag_emit(decl->loc, DIAG_ERROR, "function '%s' must return a value of type '%s'",
                  fn_name, value_type_name(decl->return_type));

    if (ret_ctx.has_return && decl->has_return_type && ret_ctx.return_type != decl->return_type)
        diag_emit(call_loc, DIAG_ERROR, "function '%s' returns '%s', expected '%s'",
                  fn_name, value_type_name(ret_ctx.return_type), value_type_name(decl->return_type));

    if (ret_value) *ret_value = ret_ctx.return_value;
    else free(ret_ctx.return_value);
    if (ret_len) *ret_len = ret_ctx.return_len;
    if (ret_type) *ret_type = ret_ctx.has_return ? ret_ctx.return_type : VAL_VOID;

    sym_table_free(&local_st);
    free(final_values); free(final_lengths); free(final_types); free(final_filled);
    return 0;
}

/* ================================================================
 * Main codegen entry point
 * ================================================================ */

/* Helper: collect fn/class declarations from an AST into tables */
static void collect_declarations(ASTNode *ast, FnTable *fn_table, ClassTable *class_table) {
    for (ASTNode *n = ast; n; n = n->next) {
        if (n->type == NODE_FN_DECL) {
            if (!fn_table_find(fn_table, n->fn_name))
                fn_table_add(fn_table, n->fn_name, n);
        }
        if (n->type == NODE_CLASS_DECL) {
            if (!class_table_find(class_table, n->class_name)) {
                int total_fields = 0;
                char **all_names = NULL;
                ValueType *all_types = NULL;

                if (n->parent_class_name) {
                    ClassDef *parent = class_table_find(class_table, n->parent_class_name);
                    if (parent) {
                        total_fields = parent->field_count + n->class_field_count;
                        all_names = malloc(total_fields * sizeof(char *));
                        all_types = malloc(total_fields * sizeof(ValueType));
                        for (int i = 0; i < parent->field_count; i++) {
                            all_names[i] = parent->field_names[i];
                            all_types[i] = parent->field_types[i];
                        }
                        for (int i = 0; i < n->class_field_count; i++) {
                            all_names[parent->field_count + i] = n->class_fields[i].name;
                            all_types[parent->field_count + i] = n->class_fields[i].type;
                        }
                    } else {
                        diag_emit(n->loc, DIAG_ERROR, "undefined parent class '%s'", n->parent_class_name);
                    }
                } else {
                    total_fields = n->class_field_count;
                    all_names = malloc(total_fields * sizeof(char *));
                    all_types = malloc(total_fields * sizeof(ValueType));
                    for (int i = 0; i < n->class_field_count; i++) {
                        all_names[i] = n->class_fields[i].name;
                        all_types[i] = n->class_fields[i].type;
                    }
                }

                if (class_table->count == class_table->cap) {
                    class_table->cap *= 2;
                    class_table->entries = realloc(class_table->entries, class_table->cap * sizeof(ClassDef));
                }
                ClassDef *cd = &class_table->entries[class_table->count++];
                cd->name = n->class_name;
                cd->parent_name = n->parent_class_name;
                cd->field_names = all_names;
                cd->field_types = all_types;
                cd->field_count = total_fields;
                cd->methods = n->class_methods;
                cd->loc = n->loc;
            }
        }
    }
}

/* Imported variable entry — stores evaluated values from imported modules */
typedef struct {
    char *name;
    EvalResult val;
    int is_const;
} ImportedVar;

/* Process all imports for an AST, recursively handling transitive imports.
   source_file: absolute path of the file being processed.
   Populates fn_table, class_table, and imp_vars with imported symbols. */
static void process_imports(ASTNode *ast, const char *source_file,
                            FnTable *fn_table, ClassTable *class_table,
                            ImportedVar **imp_vars, int *imp_var_count, int *imp_var_cap) {
    for (ASTNode *n = ast; n; n = n->next) {
        if (n->type != NODE_IMPORT) continue;

        /* Handle stdlib modules (resolved in-compiler, no file needed) */
        if (strcmp(n->import_path, "std/string") == 0) {
            for (int i = 0; i < n->import_name_count; i++) {
                const char *name = n->import_names[i];
                if (stdlib_fn_index(name) < 0)
                    diag_emit(n->loc, DIAG_ERROR, "'%s' not found in module '%s'",
                              name, n->import_path);
                stdlib_fn_import(name);
            }
            continue;
        }

        ASTNode *imported_ast = NULL;
        const char *imported_source = NULL;
        const char *imported_filename = NULL;

        if (import_resolve(source_file, n->import_path, n->loc,
                           &imported_ast, &imported_source, &imported_filename) != 0) {
            continue;
        }

        /* Save/restore diagnostic context for the imported file */
        DiagContext saved_ctx = diag_save();
        diag_init(imported_filename, imported_source);

        /* Build temporary tables for the imported file */
        FnTable imp_ft;
        fn_table_init(&imp_ft);
        ClassTable imp_ct;
        class_table_init(&imp_ct);

        /* Recursively process the imported file's own imports first */
        ImportedVar *nested_vars = malloc(8 * sizeof(ImportedVar));
        int nested_var_count = 0, nested_var_cap = 8;
        import_push_file(imported_filename);
        process_imports(imported_ast, imported_filename, &imp_ft, &imp_ct,
                        &nested_vars, &nested_var_count, &nested_var_cap);
        import_pop_file();

        collect_declarations(imported_ast, &imp_ft, &imp_ct);

        /* Evaluate the imported file to resolve its top-level symbols */
        SymTable imp_st;
        sym_table_init(&imp_st);

        /* Add nested imported variables */
        for (int i = 0; i < nested_var_count; i++) {
            sym_add(&imp_st, nested_vars[i].name, nested_vars[i].val,
                    nested_vars[i].is_const, (SourceLoc){0, 0});
            imp_st.syms[imp_st.count - 1].mutated = 1;
        }

        PrintList imp_prints;
        print_list_init(&imp_prints);

        FnTable *save_ft = g_ft;
        ClassTable *save_ct = g_ct;
        PrintList *save_prints = g_prints;
        g_ft = &imp_ft;
        g_ct = &imp_ct;
        g_prints = &imp_prints;

        eval_stmts(imported_ast, &imp_st, &imp_ft, &imp_ct, &imp_prints, NULL);

        g_ft = save_ft;
        g_ct = save_ct;
        g_prints = save_prints;

        diag_restore(saved_ctx);

        /* Copy requested symbols into the caller's tables */
        for (int i = 0; i < n->import_name_count; i++) {
            const char *name = n->import_names[i];
            int found = 0;

            for (ASTNode *imp_n = imported_ast; imp_n; imp_n = imp_n->next) {
                if (imp_n->type == NODE_FN_DECL && strcmp(imp_n->fn_name, name) == 0) {
                    if (!imp_n->is_pub)
                        diag_emit(n->loc, DIAG_ERROR, "'%s' is not public in module '%s'",
                                  name, n->import_path);
                    if (fn_table_find(fn_table, name))
                        diag_emit(n->loc, DIAG_ERROR, "duplicate symbol '%s' from import", name);
                    fn_table_add(fn_table, imp_n->fn_name, imp_n);
                    found = 1;
                    break;
                }
                if (imp_n->type == NODE_CLASS_DECL && strcmp(imp_n->class_name, name) == 0) {
                    if (!imp_n->is_pub)
                        diag_emit(n->loc, DIAG_ERROR, "'%s' is not public in module '%s'",
                                  name, n->import_path);
                    ClassDef *imp_cd = class_table_find(&imp_ct, name);
                    if (imp_cd && !class_table_find(class_table, name)) {
                        if (class_table->count == class_table->cap) {
                            class_table->cap *= 2;
                            class_table->entries = realloc(class_table->entries, class_table->cap * sizeof(ClassDef));
                        }
                        class_table->entries[class_table->count++] = *imp_cd;
                    }
                    found = 1;
                    break;
                }
            }

            if (!found) {
                for (ASTNode *imp_n = imported_ast; imp_n; imp_n = imp_n->next) {
                    if (imp_n->type == NODE_VAR_DECL && strcmp(imp_n->var_name, name) == 0) {
                        if (!imp_n->is_pub)
                            diag_emit(n->loc, DIAG_ERROR, "'%s' is not public in module '%s'",
                                      name, n->import_path);
                        Symbol *sym = sym_find(&imp_st, name);
                        if (sym) {
                            if (*imp_var_count == *imp_var_cap) {
                                *imp_var_cap *= 2;
                                *imp_vars = realloc(*imp_vars, *imp_var_cap * sizeof(ImportedVar));
                            }
                            (*imp_vars)[*imp_var_count].name = strdup(name);
                            (*imp_vars)[*imp_var_count].val = sym->val;
                            (*imp_vars)[*imp_var_count].is_const = sym->is_const;
                            (*imp_var_count)++;
                        }
                        found = 1;
                        break;
                    }
                }
            }

            if (!found)
                diag_emit(n->loc, DIAG_ERROR, "'%s' not found in module '%s'",
                          name, n->import_path);
        }

        /* Clean up nested vars names (values are copied) */
        for (int i = 0; i < nested_var_count; i++)
            free(nested_vars[i].name);
        free(nested_vars);

        print_list_free(&imp_prints);
        fn_table_free(&imp_ft);
        class_table_free(&imp_ct);
        sym_table_free(&imp_st);
    }
}

int codegen(ASTNode *ast, const char *output_path, const char *source_file) {
    stdlib_reset();

    /* Pass 0: process imports */
    if (source_file) {
        char *source_copy = strdup(source_file);
        char *dir = dirname(source_copy);
        import_init(dir);
        free(source_copy);
    }

    /* First pass: collect function and class declarations */
    FnTable fn_table;
    fn_table_init(&fn_table);

    ClassTable class_table;
    class_table_init(&class_table);

    /* Imported variables list */
    int imp_var_cap = 8;
    int imp_var_count = 0;
    ImportedVar *imp_vars = malloc(imp_var_cap * sizeof(ImportedVar));

    /* Process imports recursively */
    if (source_file) {
        import_push_file(source_file);
        process_imports(ast, source_file, &fn_table, &class_table,
                        &imp_vars, &imp_var_count, &imp_var_cap);
        import_pop_file();
    }

    for (ASTNode *n = ast; n; n = n->next) {
        if (n->type == NODE_FN_DECL) {
            if (fn_table_find(&fn_table, n->fn_name))
                diag_emit(n->loc, DIAG_ERROR, "duplicate function '%s'", n->fn_name);
            fn_table_add(&fn_table, n->fn_name, n);
        }
        if (n->type == NODE_CLASS_DECL) {
            if (class_table_find(&class_table, n->class_name))
                diag_emit(n->loc, DIAG_ERROR, "duplicate class '%s'", n->class_name);

            /* Flatten parent + own fields for inheritance */
            int total_fields = 0;
            char **all_names = NULL;
            ValueType *all_types = NULL;

            if (n->parent_class_name) {
                ClassDef *parent = class_table_find(&class_table, n->parent_class_name);
                if (!parent)
                    diag_emit(n->loc, DIAG_ERROR, "undefined parent class '%s'", n->parent_class_name);
                total_fields = parent->field_count + n->class_field_count;
                all_names = malloc(total_fields * sizeof(char *));
                all_types = malloc(total_fields * sizeof(ValueType));
                for (int i = 0; i < parent->field_count; i++) {
                    all_names[i] = parent->field_names[i];
                    all_types[i] = parent->field_types[i];
                }
                for (int i = 0; i < n->class_field_count; i++) {
                    all_names[parent->field_count + i] = n->class_fields[i].name;
                    all_types[parent->field_count + i] = n->class_fields[i].type;
                }
            } else {
                total_fields = n->class_field_count;
                all_names = malloc(total_fields * sizeof(char *));
                all_types = malloc(total_fields * sizeof(ValueType));
                for (int i = 0; i < n->class_field_count; i++) {
                    all_names[i] = n->class_fields[i].name;
                    all_types[i] = n->class_fields[i].type;
                }
            }

            if (class_table.count == class_table.cap) {
                class_table.cap *= 2;
                class_table.entries = realloc(class_table.entries, class_table.cap * sizeof(ClassDef));
            }
            ClassDef *cd = &class_table.entries[class_table.count++];
            cd->name = n->class_name;
            cd->parent_name = n->parent_class_name;
            cd->field_names = all_names;
            cd->field_types = all_types;
            cd->field_count = total_fields;
            cd->methods = n->class_methods;
            cd->loc = n->loc;
        }
    }

    SymTable st;
    sym_table_init(&st);

    /* Add imported variables to the main symbol table */
    for (int i = 0; i < imp_var_count; i++) {
        sym_add(&st, imp_vars[i].name, imp_vars[i].val, imp_vars[i].is_const, (SourceLoc){0, 0});
        /* Mark as mutated to suppress "never mutated" warning for imports */
        st.syms[st.count - 1].mutated = 1;
    }

    PrintList prints;
    print_list_init(&prints);

    /* Set globals for EXPR_FN_CALL evaluation */
    g_ft = &fn_table;
    g_ct = &class_table;
    g_prints = &prints;

    eval_stmts(ast, &st, &fn_table, &class_table, &prints, NULL);

    for (int j = 0; j < st.count; j++) {
        if (!st.syms[j].is_const && !st.syms[j].mutated)
            diag_emit(st.syms[j].loc, DIAG_WARNING, "variable '%s' is never mutated, consider using 'const'", st.syms[j].name);
    }

    sym_table_free(&st);
    fn_table_free(&fn_table);
    class_table_free(&class_table);

    /* Build string data from dynamic print list */
    int string_count = prints.count;
    int *str_offsets = malloc(string_count * sizeof(int));
    int *str_lengths_arr = malloc(string_count * sizeof(int));

    Buffer strings;
    buf_init(&strings);

    for (int i = 0; i < string_count; i++) {
        str_offsets[i] = strings.len;
        str_lengths_arr[i] = prints.lengths[i];
        buf_write(&strings, prints.strings[i], prints.lengths[i]);
    }

    print_list_free(&prints);

    int result = emit_binary(string_count, str_offsets, str_lengths_arr,
                             &strings, output_path);

    free(str_offsets);
    free(str_lengths_arr);
    buf_free(&strings);

    /* Clean up imported variables */
    for (int i = 0; i < imp_var_count; i++)
        free(imp_vars[i].name);
    free(imp_vars);

    /* Clean up import module cache (must be after codegen since fn_table
       entries may point into cached ASTs) */
    if (source_file)
        import_cleanup();

    return result;
}
