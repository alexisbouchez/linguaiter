#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *value_type_name(ValueType vt) {
    switch (vt) {
        case VAL_STRING: return "string";
        case VAL_INT:    return "int";
        case VAL_FLOAT:  return "float";
        case VAL_BOOL:   return "bool";
        default:         return "unknown";
    }
}

/* Process escape sequences in a string token, returning a new malloc'd buffer.
   Sets *out_len to the length of the processed string. */
static char *process_escapes(const char *raw, int raw_len, int *out_len) {
    char *buf = malloc(raw_len + 1);
    int j = 0;
    for (int i = 0; i < raw_len; i++) {
        if (raw[i] == '\\' && i + 1 < raw_len) {
            i++;
            switch (raw[i]) {
                case 'n':  buf[j++] = '\n'; break;
                case 't':  buf[j++] = '\t'; break;
                case 'r':  buf[j++] = '\r'; break;
                case '\\': buf[j++] = '\\'; break;
                case '"':  buf[j++] = '"';  break;
                case '0':  buf[j++] = '\0'; break;
                default:
                    fprintf(stderr, "error: unknown escape sequence '\\%c'\n", raw[i]);
                    exit(1);
            }
        } else {
            buf[j++] = raw[i];
        }
    }
    buf[j] = '\0';
    *out_len = j;
    return buf;
}

/* Parse a value token (string, int, or float).
   Returns the processed string in *out and its length in *out_len.
   Sets *out_vtype to the value type. */
static void parse_value(Lexer *lexer, char **out, int *out_len, ValueType *out_vtype) {
    Token tok = lexer_next(lexer);
    if (tok.type == TOKEN_STRING) {
        *out = process_escapes(tok.start, tok.length, out_len);
        *out_vtype = VAL_STRING;
    } else if (tok.type == TOKEN_INT) {
        *out = malloc(tok.length + 1);
        memcpy(*out, tok.start, tok.length);
        (*out)[tok.length] = '\0';
        *out_len = tok.length;
        *out_vtype = VAL_INT;
    } else if (tok.type == TOKEN_FLOAT) {
        *out = malloc(tok.length + 1);
        memcpy(*out, tok.start, tok.length);
        (*out)[tok.length] = '\0';
        *out_len = tok.length;
        *out_vtype = VAL_FLOAT;
    } else if (tok.type == TOKEN_BOOL) {
        *out = malloc(tok.length + 1);
        memcpy(*out, tok.start, tok.length);
        (*out)[tok.length] = '\0';
        *out_len = tok.length;
        *out_vtype = VAL_BOOL;
    } else {
        fprintf(stderr, "error: expected value (string, int, float, or bool)\n");
        exit(1);
    }
}

static Token expect(Lexer *lexer, TokenType type, const char *what) {
    Token tok = lexer_next(lexer);
    if (tok.type != type) {
        fprintf(stderr, "error: expected %s\n", what);
        exit(1);
    }
    return tok;
}

ASTNode *parse(Lexer *lexer) {
    ASTNode *head = NULL;
    ASTNode *tail = NULL;

    for (;;) {
        Token tok = lexer_next(lexer);
        if (tok.type == TOKEN_EOF)
            break;

        int is_const = (tok.type == TOKEN_IDENT && tok.length == 5 &&
                        memcmp(tok.start, "const", 5) == 0);
        int is_var   = (tok.type == TOKEN_IDENT && tok.length == 3 &&
                        memcmp(tok.start, "var", 3) == 0);

        if (is_const || is_var) {
            /* const/var <ident> [: <type>] = <value> ; */
            Token name = expect(lexer, TOKEN_IDENT, "variable name");

            int has_annotation = 0;
            ValueType annotated_type = VAL_STRING;

            Token after_name = lexer_next(lexer);
            if (after_name.type == TOKEN_COLON) {
                /* Parse type annotation */
                Token type_tok = expect(lexer, TOKEN_IDENT, "type name");
                has_annotation = 1;
                if (type_tok.length == 3 && memcmp(type_tok.start, "int", 3) == 0) {
                    annotated_type = VAL_INT;
                } else if (type_tok.length == 5 && memcmp(type_tok.start, "float", 5) == 0) {
                    annotated_type = VAL_FLOAT;
                } else if (type_tok.length == 6 && memcmp(type_tok.start, "string", 6) == 0) {
                    annotated_type = VAL_STRING;
                } else if (type_tok.length == 4 && memcmp(type_tok.start, "bool", 4) == 0) {
                    annotated_type = VAL_BOOL;
                } else {
                    fprintf(stderr, "error: unknown type '%.*s'\n", type_tok.length, type_tok.start);
                    exit(1);
                }
                expect(lexer, TOKEN_EQUALS, "'='");
            } else if (after_name.type != TOKEN_EQUALS) {
                fprintf(stderr, "error: expected ':' or '='\n");
                exit(1);
            }

            ASTNode *node = malloc(sizeof(ASTNode));
            node->type = NODE_VAR_DECL;
            node->is_const = is_const;
            node->var_name = malloc(name.length + 1);
            memcpy(node->var_name, name.start, name.length);
            node->var_name[name.length] = '\0';
            parse_value(lexer, &node->string, &node->string_len, &node->value_type);
            node->is_var_ref = 0;
            node->next = NULL;

            if (has_annotation && annotated_type != node->value_type) {
                fprintf(stderr, "error: type mismatch: variable '%s' declared as '%s', but assigned '%s'\n",
                        node->var_name, value_type_name(annotated_type), value_type_name(node->value_type));
                exit(1);
            }

            expect(lexer, TOKEN_SEMICOLON, "';'");

            if (tail) {
                tail->next = node;
            } else {
                head = node;
            }
            tail = node;
        } else if (tok.type == TOKEN_IDENT && tok.length == 5 &&
            memcmp(tok.start, "print", 5) == 0) {
            expect(lexer, TOKEN_LPAREN, "'('");

            Token arg = lexer_next(lexer);
            ASTNode *node = malloc(sizeof(ASTNode));
            node->type = NODE_PRINT;
            node->next = NULL;
            node->value_type = VAL_STRING;

            if (arg.type == TOKEN_STRING) {
                node->string = process_escapes(arg.start, arg.length, &node->string_len);
                node->var_name = NULL;
                node->is_var_ref = 0;
            } else if (arg.type == TOKEN_INT || arg.type == TOKEN_FLOAT || arg.type == TOKEN_BOOL) {
                node->string = malloc(arg.length + 1);
                memcpy(node->string, arg.start, arg.length);
                node->string[arg.length] = '\0';
                node->string_len = arg.length;
                node->var_name = NULL;
                node->is_var_ref = 0;
                node->value_type = (arg.type == TOKEN_INT) ? VAL_INT :
                                   (arg.type == TOKEN_FLOAT) ? VAL_FLOAT : VAL_BOOL;
            } else if (arg.type == TOKEN_IDENT) {
                node->string = NULL;
                node->string_len = 0;
                node->var_name = malloc(arg.length + 1);
                memcpy(node->var_name, arg.start, arg.length);
                node->var_name[arg.length] = '\0';
                node->is_var_ref = 1;
            } else {
                fprintf(stderr, "error: expected value or variable name\n");
                exit(1);
            }

            expect(lexer, TOKEN_RPAREN, "')'");
            expect(lexer, TOKEN_SEMICOLON, "';'");

            if (tail) {
                tail->next = node;
            } else {
                head = node;
            }
            tail = node;
        } else if (tok.type == TOKEN_IDENT) {
            /* <ident> = <value> ; (assignment) */
            expect(lexer, TOKEN_EQUALS, "'='");

            ASTNode *node = malloc(sizeof(ASTNode));
            node->type = NODE_ASSIGN;
            node->var_name = malloc(tok.length + 1);
            memcpy(node->var_name, tok.start, tok.length);
            node->var_name[tok.length] = '\0';
            parse_value(lexer, &node->string, &node->string_len, &node->value_type);
            node->is_var_ref = 0;
            node->is_const = 0;
            node->next = NULL;
            expect(lexer, TOKEN_SEMICOLON, "';'");

            if (tail) {
                tail->next = node;
            } else {
                head = node;
            }
            tail = node;
        } else {
            fprintf(stderr, "error: unexpected token\n");
            exit(1);
        }
    }

    return head;
}

void ast_free(ASTNode *node) {
    while (node) {
        ASTNode *next = node->next;
        free(node->string);
        free(node->var_name);
        free(node);
        node = next;
    }
}
