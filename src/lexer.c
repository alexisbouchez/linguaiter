#include "lexer.h"
#include <ctype.h>

void lexer_init(Lexer *l, const char *source) {
    l->source = source;
    l->pos = 0;
}

static void skip_whitespace(Lexer *l) {
    while (l->source[l->pos] && isspace(l->source[l->pos]))
        l->pos++;
}

Token lexer_next(Lexer *l) {
    skip_whitespace(l);

    Token tok;
    char c = l->source[l->pos];

    if (c == '\0') {
        tok.type = TOKEN_EOF;
        tok.start = &l->source[l->pos];
        tok.length = 0;
        return tok;
    }

    if (c == '(') {
        tok.type = TOKEN_LPAREN;
        tok.start = &l->source[l->pos];
        tok.length = 1;
        l->pos++;
        return tok;
    }

    if (c == ')') {
        tok.type = TOKEN_RPAREN;
        tok.start = &l->source[l->pos];
        tok.length = 1;
        l->pos++;
        return tok;
    }

    if (c == ';') {
        tok.type = TOKEN_SEMICOLON;
        tok.start = &l->source[l->pos];
        tok.length = 1;
        l->pos++;
        return tok;
    }

    if (c == '=') {
        tok.type = TOKEN_EQUALS;
        tok.start = &l->source[l->pos];
        tok.length = 1;
        l->pos++;
        return tok;
    }

    if (c == ':') {
        tok.type = TOKEN_COLON;
        tok.start = &l->source[l->pos];
        tok.length = 1;
        l->pos++;
        return tok;
    }

    if (c == '"') {
        l->pos++; /* skip opening quote */
        int start = l->pos;
        while (l->source[l->pos] && l->source[l->pos] != '"')
            l->pos++;
        tok.type = TOKEN_STRING;
        tok.start = &l->source[start];
        tok.length = l->pos - start;
        if (l->source[l->pos] == '"')
            l->pos++; /* skip closing quote */
        return tok;
    }

    if (isdigit(c)) {
        int start = l->pos;
        while (isdigit(l->source[l->pos]))
            l->pos++;
        if (l->source[l->pos] == '.' && isdigit(l->source[l->pos + 1])) {
            l->pos++; /* skip '.' */
            while (isdigit(l->source[l->pos]))
                l->pos++;
            tok.type = TOKEN_FLOAT;
        } else {
            tok.type = TOKEN_INT;
        }
        tok.start = &l->source[start];
        tok.length = l->pos - start;
        return tok;
    }

    if (isalpha(c) || c == '_') {
        int start = l->pos;
        while (isalnum(l->source[l->pos]) || l->source[l->pos] == '_')
            l->pos++;
        tok.type = TOKEN_IDENT;
        tok.start = &l->source[start];
        tok.length = l->pos - start;
        return tok;
    }

    /* Unknown character — skip and return EOF */
    tok.type = TOKEN_EOF;
    tok.start = &l->source[l->pos];
    tok.length = 0;
    l->pos++;
    return tok;
}
