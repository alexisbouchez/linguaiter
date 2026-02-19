#include "codegen.h"
#include "codegen/codegen_internal.h"

/* If no target backend matched, fail at link time with a clear message. */
#if !defined(__linux__) || !defined(__x86_64__)
#if !defined(__APPLE__) || !defined(__aarch64__)
/* Provide a stub so the compiler still reports the real problem. */
int emit_binary(int string_count, int *str_offsets, int *str_lengths_arr,
                Buffer *strings, const char *output_path)
{
    (void)string_count; (void)str_offsets; (void)str_lengths_arr;
    (void)strings; (void)output_path;
    fprintf(stderr, "error: no codegen backend for this platform\n");
    return 1;
}
#endif
#endif

int codegen(ASTNode *ast, const char *output_path) {
    /* Pre-pass: resolve variables sequentially (handles reassignment) */
    int sym_cap = 16;
    int sym_count = 0;
    char **sym_names = malloc(sym_cap * sizeof(char *));
    char **sym_values = malloc(sym_cap * sizeof(char *));
    int *sym_lengths = malloc(sym_cap * sizeof(int));
    int *sym_is_const = malloc(sym_cap * sizeof(int));
    int *sym_mutated = malloc(sym_cap * sizeof(int));
    int *sym_value_types = malloc(sym_cap * sizeof(int));

    for (ASTNode *n = ast; n; n = n->next) {
        if (n->type == NODE_VAR_DECL) {
            if (sym_count == sym_cap) {
                sym_cap *= 2;
                sym_names = realloc(sym_names, sym_cap * sizeof(char *));
                sym_values = realloc(sym_values, sym_cap * sizeof(char *));
                sym_lengths = realloc(sym_lengths, sym_cap * sizeof(int));
                sym_is_const = realloc(sym_is_const, sym_cap * sizeof(int));
                sym_mutated = realloc(sym_mutated, sym_cap * sizeof(int));
                sym_value_types = realloc(sym_value_types, sym_cap * sizeof(int));
            }
            sym_names[sym_count] = n->var_name;
            sym_values[sym_count] = n->string;
            sym_lengths[sym_count] = n->string_len;
            sym_is_const[sym_count] = n->is_const;
            sym_mutated[sym_count] = 0;
            sym_value_types[sym_count] = n->value_type;
            sym_count++;
        } else if (n->type == NODE_ASSIGN) {
            int found = 0;
            for (int j = 0; j < sym_count; j++) {
                if (strcmp(n->var_name, sym_names[j]) == 0) {
                    if (sym_is_const[j]) {
                        fprintf(stderr, "error: cannot reassign const variable '%s'\n", n->var_name);
                        free(sym_names); free(sym_values); free(sym_lengths); free(sym_is_const); free(sym_mutated); free(sym_value_types);
                        return 1;
                    }
                    if (sym_value_types[j] != (int)n->value_type) {
                        fprintf(stderr, "error: type mismatch: variable '%s' has type '%s', cannot assign '%s'\n",
                                n->var_name, value_type_name(sym_value_types[j]), value_type_name(n->value_type));
                        free(sym_names); free(sym_values); free(sym_lengths); free(sym_is_const); free(sym_mutated); free(sym_value_types);
                        return 1;
                    }
                    sym_values[j] = n->string;
                    sym_lengths[j] = n->string_len;
                    sym_mutated[j] = 1;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "error: undefined variable '%s'\n", n->var_name);
                free(sym_names); free(sym_values); free(sym_lengths); free(sym_is_const); free(sym_mutated); free(sym_value_types);
                return 1;
            }
        } else if (n->type == NODE_PRINT && n->is_var_ref) {
            int found = 0;
            for (int j = 0; j < sym_count; j++) {
                if (strcmp(n->var_name, sym_names[j]) == 0) {
                    n->string = malloc(sym_lengths[j] + 1);
                    memcpy(n->string, sym_values[j], sym_lengths[j]);
                    n->string[sym_lengths[j]] = '\0';
                    n->string_len = sym_lengths[j];
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "error: undefined variable '%s'\n", n->var_name);
                free(sym_names); free(sym_values); free(sym_lengths); free(sym_is_const); free(sym_mutated); free(sym_value_types);
                return 1;
            }
        }
    }

    for (int j = 0; j < sym_count; j++) {
        if (!sym_is_const[j] && !sym_mutated[j])
            fprintf(stderr, "\033[1;33mwarning:\033[0m variable '\033[1m%s\033[0m' is never mutated, consider using '\033[1mconst\033[0m'\n", sym_names[j]);
    }

    free(sym_names);
    free(sym_values);
    free(sym_lengths);
    free(sym_is_const);
    free(sym_mutated);
    free(sym_value_types);

    /* Collect strings from print statements */
    int string_count = 0;
    for (ASTNode *n = ast; n; n = n->next)
        if (n->type == NODE_PRINT)
            string_count++;

    int *str_offsets = malloc(string_count * sizeof(int));
    int *str_lengths_arr = malloc(string_count * sizeof(int));

    Buffer strings;
    buf_init(&strings);

    int i = 0;
    for (ASTNode *n = ast; n; n = n->next) {
        if (n->type == NODE_PRINT) {
            str_offsets[i] = strings.len;
            str_lengths_arr[i] = n->string_len;
            buf_write(&strings, n->string, n->string_len);
            i++;
        }
    }

    /* Emit platform-specific binary */
    int result = emit_binary(string_count, str_offsets, str_lengths_arr,
                             &strings, output_path);

    free(str_offsets);
    free(str_lengths_arr);
    buf_free(&strings);

    return result;
}
