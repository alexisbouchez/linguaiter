#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

        if (tok.type == TOKEN_IDENT && tok.length == 5 &&
            memcmp(tok.start, "print", 5) == 0) {
            expect(lexer, TOKEN_LPAREN, "'('");
            Token str = expect(lexer, TOKEN_STRING, "string literal");
            expect(lexer, TOKEN_RPAREN, "')'");
            expect(lexer, TOKEN_SEMICOLON, "';'");

            ASTNode *node = malloc(sizeof(ASTNode));
            node->type = NODE_PRINT;
            node->string = malloc(str.length + 1);
            memcpy(node->string, str.start, str.length);
            node->string[str.length] = '\0';
            node->string_len = str.length;
            node->next = NULL;

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
        free(node);
        node = next;
    }
}
