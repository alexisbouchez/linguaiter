#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

/* Buffer helpers */
typedef struct {
    uint8_t *data;
    int len;
    int cap;
} Buffer;

static void buf_init(Buffer *b) {
    b->cap = 16384;
    b->data = calloc(1, b->cap);
    b->len = 0;
}

static void buf_ensure(Buffer *b, int need) {
    while (b->len + need > b->cap) {
        b->cap *= 2;
        b->data = realloc(b->data, b->cap);
        memset(b->data + b->len, 0, b->cap - b->len);
    }
}

static void buf_write(Buffer *b, const void *data, int len) {
    buf_ensure(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void buf_write8(Buffer *b, uint8_t val) {
    buf_write(b, &val, 1);
}

static void buf_write16(Buffer *b, uint16_t val) {
    buf_write(b, &val, 2);
}

static void buf_write32(Buffer *b, uint32_t val) {
    buf_write(b, &val, 4);
}

static void buf_write64(Buffer *b, uint64_t val) {
    buf_write(b, &val, 8);
}

/* ELF constants */
#define ELF_HEADER_SIZE  64
#define PHDR_SIZE        56
#define CODE_OFFSET      (ELF_HEADER_SIZE + PHDR_SIZE)  /* 0x78 = 120 */
#define BASE_ADDR        0x400000ULL
#define ENTRY_ADDR       (BASE_ADDR + CODE_OFFSET)

/* x86-64 instruction sizes */
#define PRINT_INSTR_SIZE 24  /* per print statement */
#define EXIT_INSTR_SIZE  9   /* exit(0) sequence */

int codegen(ASTNode *ast, const char *output_path) {
    /* Pre-pass: resolve variables sequentially (handles reassignment) */
    int sym_cap = 16;
    int sym_count = 0;
    char **sym_names = malloc(sym_cap * sizeof(char *));
    char **sym_values = malloc(sym_cap * sizeof(char *));
    int *sym_lengths = malloc(sym_cap * sizeof(int));
    int *sym_is_const = malloc(sym_cap * sizeof(int));

    for (ASTNode *n = ast; n; n = n->next) {
        if (n->type == NODE_VAR_DECL) {
            if (sym_count == sym_cap) {
                sym_cap *= 2;
                sym_names = realloc(sym_names, sym_cap * sizeof(char *));
                sym_values = realloc(sym_values, sym_cap * sizeof(char *));
                sym_lengths = realloc(sym_lengths, sym_cap * sizeof(int));
                sym_is_const = realloc(sym_is_const, sym_cap * sizeof(int));
            }
            sym_names[sym_count] = n->var_name;
            sym_values[sym_count] = n->string;
            sym_lengths[sym_count] = n->string_len;
            sym_is_const[sym_count] = n->is_const;
            sym_count++;
        } else if (n->type == NODE_ASSIGN) {
            int found = 0;
            for (int j = 0; j < sym_count; j++) {
                if (strcmp(n->var_name, sym_names[j]) == 0) {
                    if (sym_is_const[j]) {
                        fprintf(stderr, "error: cannot reassign const variable '%s'\n", n->var_name);
                        free(sym_names); free(sym_values); free(sym_lengths); free(sym_is_const);
                        return 1;
                    }
                    sym_values[j] = n->string;
                    sym_lengths[j] = n->string_len;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "error: undefined variable '%s'\n", n->var_name);
                free(sym_names); free(sym_values); free(sym_lengths); free(sym_is_const);
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
                free(sym_names); free(sym_values); free(sym_lengths); free(sym_is_const);
                return 1;
            }
        }
    }

    free(sym_names);
    free(sym_values);
    free(sym_lengths);
    free(sym_is_const);

    /* First pass: collect strings from print statements */
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

    /*
     * Generate x86-64 code.
     *
     * Per print statement (24 bytes):
     *   mov eax, 1              ; B8 01 00 00 00   (sys_write)
     *   mov edi, 1              ; BF 01 00 00 00   (stdout)
     *   lea rsi, [rip+disp32]   ; 48 8D 35 XX XX XX XX
     *   mov edx, <len>          ; BA XX XX XX XX
     *   syscall                 ; 0F 05
     *
     * Exit (9 bytes):
     *   mov eax, 60             ; B8 3C 00 00 00   (sys_exit)
     *   xor edi, edi            ; 31 FF             (status 0)
     *   syscall                 ; 0F 05
     */
    int total_instr_size = string_count * PRINT_INSTR_SIZE + EXIT_INSTR_SIZE;

    Buffer code;
    buf_init(&code);

    i = 0;
    for (ASTNode *n = ast; n; n = n->next) {
        if (n->type != NODE_PRINT) continue;

        /*
         * LEA is at code offset: i*24 + 10
         * RIP after LEA:         i*24 + 17
         * String target:         total_instr_size + str_offsets[i]
         * disp32 = target - rip_after_lea
         */
        int32_t rip_after_lea = i * PRINT_INSTR_SIZE + 17;
        int32_t target = total_instr_size + str_offsets[i];
        int32_t disp = target - rip_after_lea;

        /* mov eax, 1 */
        buf_write8(&code, 0xB8);
        buf_write32(&code, 1);

        /* mov edi, 1 */
        buf_write8(&code, 0xBF);
        buf_write32(&code, 1);

        /* lea rsi, [rip+disp32] */
        buf_write8(&code, 0x48);
        buf_write8(&code, 0x8D);
        buf_write8(&code, 0x35);
        buf_write32(&code, (uint32_t)disp);

        /* mov edx, <len> */
        buf_write8(&code, 0xBA);
        buf_write32(&code, (uint32_t)str_lengths_arr[i]);

        /* syscall */
        buf_write8(&code, 0x0F);
        buf_write8(&code, 0x05);

        i++;
    }

    /* Exit: mov eax, 60; xor edi, edi; syscall */
    buf_write8(&code, 0xB8);
    buf_write32(&code, 60);
    buf_write8(&code, 0x31);
    buf_write8(&code, 0xFF);
    buf_write8(&code, 0x0F);
    buf_write8(&code, 0x05);

    /* Append string data right after instructions */
    buf_write(&code, strings.data, strings.len);

    /* Total file size = ELF header + 1 program header + code + strings */
    uint64_t file_size = CODE_OFFSET + code.len;

    /* Build the ELF file */
    Buffer out;
    buf_init(&out);

    /* ---- ELF64 Header (64 bytes) ---- */
    /* e_ident */
    buf_write8(&out, 0x7F);           /* EI_MAG0 */
    buf_write8(&out, 'E');             /* EI_MAG1 */
    buf_write8(&out, 'L');             /* EI_MAG2 */
    buf_write8(&out, 'F');             /* EI_MAG3 */
    buf_write8(&out, 2);              /* EI_CLASS: ELFCLASS64 */
    buf_write8(&out, 1);              /* EI_DATA: ELFDATA2LSB */
    buf_write8(&out, 1);              /* EI_VERSION: EV_CURRENT */
    buf_write8(&out, 0);              /* EI_OSABI: ELFOSABI_NONE */
    buf_write64(&out, 0);             /* EI_ABIVERSION + padding (8 bytes total with prev) */

    /* e_type */
    buf_write16(&out, 2);             /* ET_EXEC */
    /* e_machine */
    buf_write16(&out, 0x3E);          /* EM_X86_64 */
    /* e_version */
    buf_write32(&out, 1);             /* EV_CURRENT */
    /* e_entry */
    buf_write64(&out, ENTRY_ADDR);
    /* e_phoff */
    buf_write64(&out, ELF_HEADER_SIZE);
    /* e_shoff */
    buf_write64(&out, 0);             /* no section headers */
    /* e_flags */
    buf_write32(&out, 0);
    /* e_ehsize */
    buf_write16(&out, ELF_HEADER_SIZE);
    /* e_phentsize */
    buf_write16(&out, PHDR_SIZE);
    /* e_phnum */
    buf_write16(&out, 1);
    /* e_shentsize */
    buf_write16(&out, 0);
    /* e_shnum */
    buf_write16(&out, 0);
    /* e_shstrndx */
    buf_write16(&out, 0);

    /* ---- Program Header: PT_LOAD (56 bytes) ---- */
    /* p_type */
    buf_write32(&out, 1);             /* PT_LOAD */
    /* p_flags */
    buf_write32(&out, 0x5);           /* PF_R | PF_X */
    /* p_offset */
    buf_write64(&out, 0);
    /* p_vaddr */
    buf_write64(&out, BASE_ADDR);
    /* p_paddr */
    buf_write64(&out, BASE_ADDR);
    /* p_filesz */
    buf_write64(&out, file_size);
    /* p_memsz */
    buf_write64(&out, file_size);
    /* p_align */
    buf_write64(&out, 0x1000);

    /* ---- Code + string data ---- */
    buf_write(&out, code.data, code.len);

    /* Write to file */
    FILE *f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s' for writing\n", output_path);
        free(code.data); free(strings.data);
        free(str_offsets); free(str_lengths_arr);
        free(out.data);
        return 1;
    }

    fwrite(out.data, 1, out.len, f);
    fclose(f);
    chmod(output_path, 0755);

    free(code.data);
    free(strings.data);
    free(str_offsets);
    free(str_lengths_arr);
    free(out.data);

    return 0;
}
