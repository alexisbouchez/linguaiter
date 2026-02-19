#ifndef LEXER_H
#define LEXER_H

typedef enum {
    TOKEN_IDENT,
    TOKEN_STRING,
    TOKEN_INT,
    TOKEN_FLOAT,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_SEMICOLON,
    TOKEN_EQUALS,
    TOKEN_COLON,
    TOKEN_EOF,
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    int length;
} Token;

typedef struct {
    const char *source;
    int pos;
} Lexer;

void lexer_init(Lexer *l, const char *source);
Token lexer_next(Lexer *l);

#endif
