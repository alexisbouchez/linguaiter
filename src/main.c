#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
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

static void help(void) {
    printf("lingua - a minimal compiler for the Lingua language\n"
           "\n"
           "Usage:\n"
           "  lingua <file>.lingua                Build and run a .lingua file\n"
           "  lingua build <file> -o <output>     Compile a .lingua file to a native binary\n"
           "  lingua completions <shell>           Generate shell completions (bash, zsh, fish)\n"
           "  lingua --help, -h                    Show this help message\n");
}

static void usage(void) {
    fprintf(stderr, "usage: lingua <file>.lingua\n");
    fprintf(stderr, "       lingua build <file> -o <output>\n");
    fprintf(stderr, "       lingua completions <shell>\n");
    fprintf(stderr, "       lingua --help\n");
    exit(1);
}

static int build(const char *input_path, const char *output_path) {
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

static int ends_with(const char *str, const char *suffix) {
    int str_len = strlen(str);
    int suf_len = strlen(suffix);
    if (suf_len > str_len)
        return 0;
    return memcmp(str + str_len - suf_len, suffix, suf_len) == 0;
}

static void completions_bash(void) {
    printf(
        "_lingua() {\n"
        "    local cur prev words cword\n"
        "    _init_completion || return\n"
        "\n"
        "    if [[ $cword -eq 1 ]]; then\n"
        "        COMPREPLY=($(compgen -W 'build completions' -- \"$cur\"))\n"
        "        return\n"
        "    fi\n"
        "\n"
        "    case \"${words[1]}\" in\n"
        "        build)\n"
        "            if [[ $prev == -o ]]; then\n"
        "                _filedir\n"
        "            elif [[ $cur == -* ]]; then\n"
        "                COMPREPLY=($(compgen -W '-o' -- \"$cur\"))\n"
        "            else\n"
        "                _filedir lingua\n"
        "            fi\n"
        "            ;;\n"
        "        completions)\n"
        "            COMPREPLY=($(compgen -W 'bash zsh fish' -- \"$cur\"))\n"
        "            ;;\n"
        "    esac\n"
        "}\n"
        "\n"
        "complete -F _lingua lingua\n"
    );
}

static void completions_zsh(void) {
    printf(
        "#compdef lingua\n"
        "\n"
        "_lingua() {\n"
        "    local -a subcmds\n"
        "    subcmds=('build:Compile a .lingua file' 'completions:Generate shell completions')\n"
        "\n"
        "    _arguments -C '1:command:->cmd' '*::arg:->args'\n"
        "\n"
        "    case $state in\n"
        "        cmd)\n"
        "            _describe 'command' subcmds\n"
        "            ;;\n"
        "        args)\n"
        "            case $words[1] in\n"
        "                build)\n"
        "                    _arguments '1:input file:_files -g \"*.lingua\"' '-o[output file]:output file:_files'\n"
        "                    ;;\n"
        "                completions)\n"
        "                    _arguments '1:shell:(bash zsh fish)'\n"
        "                    ;;\n"
        "            esac\n"
        "            ;;\n"
        "    esac\n"
        "}\n"
        "\n"
        "_lingua\n"
    );
}

static void completions_fish(void) {
    printf(
        "complete -c lingua -f\n"
        "complete -c lingua -n '__fish_use_subcommand' -a build -d 'Compile a .lingua file'\n"
        "complete -c lingua -n '__fish_use_subcommand' -a completions -d 'Generate shell completions'\n"
        "complete -c lingua -n '__fish_seen_subcommand_from build' -s o -r -F -d 'Output file'\n"
        "complete -c lingua -n '__fish_seen_subcommand_from build' -F -d 'Input .lingua file'\n"
        "complete -c lingua -n '__fish_seen_subcommand_from completions' -a 'bash zsh fish' -d 'Shell type'\n"
    );
}

int main(int argc, char **argv) {
    if (argc < 2)
        usage();

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        help();
        return 0;
    }

    if (strcmp(argv[1], "completions") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: lingua completions <bash|zsh|fish>\n");
            return 1;
        }
        if (strcmp(argv[2], "bash") == 0) completions_bash();
        else if (strcmp(argv[2], "zsh") == 0) completions_zsh();
        else if (strcmp(argv[2], "fish") == 0) completions_fish();
        else {
            fprintf(stderr, "error: unknown shell '%s' (expected bash, zsh, or fish)\n", argv[2]);
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "build") == 0) {
        if (argc < 5)
            usage();
        if (strcmp(argv[3], "-o") != 0)
            usage();
        return build(argv[2], argv[4]);
    }

    /* lingua <file>.lingua — build to a temp binary, run it, clean up */
    if (ends_with(argv[1], ".lingua")) {
        char tmp[] = "/tmp/lingua_XXXXXX";
        int fd = mkstemp(tmp);
        if (fd < 0) {
            fprintf(stderr, "error: cannot create temporary file\n");
            return 1;
        }
        close(fd);

        int rc = build(argv[1], tmp);
        if (rc != 0) {
            unlink(tmp);
            return rc;
        }

        rc = system(tmp);
        unlink(tmp);

        if (rc == -1)
            return 1;
        return WEXITSTATUS(rc);
    }

    usage();
    return 1;
}
