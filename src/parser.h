#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"

typedef enum {
    NODE_PRINT,
} NodeType;

typedef struct ASTNode {
    NodeType type;
    char *string;
    int string_len;
    struct ASTNode *next;
} ASTNode;

ASTNode *parse(Lexer *lexer);
void ast_free(ASTNode *node);

#endif
