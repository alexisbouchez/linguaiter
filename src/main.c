#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "codegen.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static void usage(void) {
    fprintf(stderr, "usage: lingua build <file> -o <output>\n");
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 5)
        usage();

    if (strcmp(argv[1], "build") != 0)
        usage();

    const char *input_path = argv[2];
    const char *output_path = NULL;

    if (strcmp(argv[3], "-o") == 0)
        output_path = argv[4];
    else
        usage();

    char *source = read_file(input_path);
    if (!source)
        return 1;

    Lexer lexer;
    lexer_init(&lexer, source);

    ASTNode *ast = parse(&lexer);
    if (!ast) {
        fprintf(stderr, "error: no statements found\n");
        free(source);
        return 1;
    }

    int result = codegen(ast, output_path);

    ast_free(ast);
    free(source);

    return result;
}
