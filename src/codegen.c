#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

/* ARM64 instruction encoding helpers */
static uint32_t movz(int rd, uint16_t imm, int shift) {
    return 0xD2800000 | ((shift / 16) << 21) | ((uint32_t)imm << 5) | rd;
}

static uint32_t adr(int rd, int32_t offset) {
    uint32_t immlo = (offset & 0x3) << 29;
    uint32_t immhi = ((offset >> 2) & 0x7FFFF) << 5;
    return 0x10000000 | immlo | immhi | rd;
}

static uint32_t svc(uint16_t imm) {
    return 0xD4000001 | ((uint32_t)imm << 5);
}

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

static void buf_write32(Buffer *b, uint32_t val) {
    buf_write(b, &val, 4);
}

static void buf_write64(Buffer *b, uint64_t val) {
    buf_write(b, &val, 8);
}

static void buf_pad_to(Buffer *b, int target) {
    if (target <= b->len) return;
    buf_ensure(b, target - b->len);
    memset(b->data + b->len, 0, target - b->len);
    b->len = target;
}

static void buf_write_segname(Buffer *b, const char *name) {
    char buf[16];
    memset(buf, 0, 16);
    strncpy(buf, name, 16);
    buf_write(b, buf, 16);
}

/* Mach-O constants */
#define MH_MAGIC_64        0xFEEDFACF
#define MH_EXECUTE         2
#define CPU_TYPE_ARM64     ((uint32_t)0x0100000C)
#define CPU_SUBTYPE_ALL    0
#define MH_NOUNDEFS        0x1
#define MH_DYLDLINK        0x4
#define MH_PIE             0x200000
#define MH_TWOLEVEL        0x80

#define LC_SEGMENT_64      0x19
#define LC_SYMTAB          0x02
#define LC_DYSYMTAB        0x0B
#define LC_LOAD_DYLIB      0x0C
#define LC_LOAD_DYLINKER   0x0E
#define LC_MAIN            ((uint32_t)(0x28 | 0x80000000))
#define LC_BUILD_VERSION   0x32
#define LC_DYLD_CHAINED_FIXUPS   ((uint32_t)(0x34 | 0x80000000))
#define LC_DYLD_EXPORTS_TRIE     ((uint32_t)(0x33 | 0x80000000))
#define LC_CODE_SIGNATURE  0x1d

#define VM_PROT_NONE       0
#define VM_PROT_READ       1
#define VM_PROT_EXECUTE    4

#define PLATFORM_MACOS     1

#define PAGE_SIZE          16384

int codegen(ASTNode *ast, const char *output_path) {
    /* First pass: collect strings */
    int string_count = 0;
    for (ASTNode *n = ast; n; n = n->next)
        string_count++;

    int *str_offsets = malloc(string_count * sizeof(int));
    int *str_lengths = malloc(string_count * sizeof(int));

    Buffer strings;
    buf_init(&strings);

    int i = 0;
    for (ASTNode *n = ast; n; n = n->next, i++) {
        str_offsets[i] = strings.len;
        str_lengths[i] = n->string_len;
        buf_write(&strings, n->string, n->string_len);
    }

    /* Generate ARM64 code */
    int instr_size = string_count * 20 + 12;

    Buffer code;
    buf_init(&code);

    i = 0;
    for (ASTNode *n = ast; n; n = n->next, i++) {
        int str_off = instr_size - code.len + str_offsets[i];
        buf_write32(&code, adr(1, str_off));
        buf_write32(&code, movz(0, 1, 0));
        buf_write32(&code, movz(2, str_lengths[i], 0));
        buf_write32(&code, movz(16, 4, 0));
        buf_write32(&code, svc(0x80));
    }

    buf_write32(&code, movz(0, 0, 0));
    buf_write32(&code, movz(16, 1, 0));
    buf_write32(&code, svc(0x80));

    buf_write(&code, strings.data, strings.len);

    /*
     * Layout: code is placed in the header page, right after load commands.
     * __TEXT segment: vmaddr=0x100000000, fileoff=0, covers the header page.
     * __LINKEDIT: starts at PAGE_SIZE, contains fixups + exports + symtab data.
     */

    uint64_t text_vmaddr = 0x100000000ULL;

    /* ---- Calculate load command sizes ---- */
    /* Total load commands we need to emit: */
    int sz_seg_nosect = 72;                          /* LC_SEGMENT_64 with 0 sections */
    int sz_seg_1sect  = 72 + 80;                     /* LC_SEGMENT_64 with 1 section */
    int sz_dylinker   = 32;                          /* 12 + "/usr/lib/dyld\0" padded to 8 = 20 */
    int sz_main       = 24;
    int sz_build_ver  = 24;                          /* no tools */
    int sz_load_dylib = 56;                          /* 24 + "/usr/lib/libSystem.B.dylib\0" padded */
    int sz_chained    = 16;                          /* LC_DYLD_CHAINED_FIXUPS */
    int sz_exports    = 16;                          /* LC_DYLD_EXPORTS_TRIE */
    int sz_symtab     = 24;
    int sz_dysymtab   = 80;

    int ncmds = 11;
    int sizeofcmds = sz_seg_nosect     /* __PAGEZERO */
                   + sz_seg_1sect      /* __TEXT */
                   + sz_seg_nosect     /* __LINKEDIT */
                   + sz_dylinker
                   + sz_main
                   + sz_build_ver
                   + sz_load_dylib
                   + sz_chained
                   + sz_exports
                   + sz_symtab
                   + sz_dysymtab;

    int header_and_cmds = 32 + sizeofcmds;
    /* Leave 32 bytes of padding for codesign to insert LC_CODE_SIGNATURE (16 bytes) */
    int code_offset = (header_and_cmds + 32 + 3) & ~3;

    uint64_t text_segment_vmsize = PAGE_SIZE;
    uint64_t text_segment_filesize = PAGE_SIZE;
    uint64_t linkedit_file_offset = PAGE_SIZE;
    uint64_t linkedit_vmaddr = text_vmaddr + text_segment_vmsize;

    /* __LINKEDIT content: chained fixups + exports trie */
    /* Chained fixups: 48 bytes (header + starts_in_image, no imports) */
    int chained_fixups_off = (int)linkedit_file_offset;
    int chained_fixups_size = 48;

    /* Exports trie: 2 bytes (empty node) padded to 8 */
    int exports_trie_off = chained_fixups_off + chained_fixups_size;
    int exports_trie_size = 8;

    int linkedit_total = chained_fixups_size + exports_trie_size;

    /* Build the Mach-O file */
    Buffer out;
    buf_init(&out);

    /* ---- Mach-O Header (32 bytes) ---- */
    buf_write32(&out, MH_MAGIC_64);
    buf_write32(&out, CPU_TYPE_ARM64);
    buf_write32(&out, CPU_SUBTYPE_ALL);
    buf_write32(&out, MH_EXECUTE);
    buf_write32(&out, ncmds);
    buf_write32(&out, sizeofcmds);
    buf_write32(&out, MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_PIE);
    buf_write32(&out, 0);

    /* ---- LC_SEGMENT_64: __PAGEZERO ---- */
    buf_write32(&out, LC_SEGMENT_64);
    buf_write32(&out, sz_seg_nosect);
    buf_write_segname(&out, "__PAGEZERO");
    buf_write64(&out, 0);
    buf_write64(&out, 0x100000000ULL);
    buf_write64(&out, 0);
    buf_write64(&out, 0);
    buf_write32(&out, VM_PROT_NONE);
    buf_write32(&out, VM_PROT_NONE);
    buf_write32(&out, 0);
    buf_write32(&out, 0);

    /* ---- LC_SEGMENT_64: __TEXT ---- */
    buf_write32(&out, LC_SEGMENT_64);
    buf_write32(&out, sz_seg_1sect);
    buf_write_segname(&out, "__TEXT");
    buf_write64(&out, text_vmaddr);
    buf_write64(&out, text_segment_vmsize);
    buf_write64(&out, 0);
    buf_write64(&out, text_segment_filesize);
    buf_write32(&out, VM_PROT_READ | VM_PROT_EXECUTE);
    buf_write32(&out, VM_PROT_READ | VM_PROT_EXECUTE);
    buf_write32(&out, 1);
    buf_write32(&out, 0);

    /* Section: __text */
    buf_write_segname(&out, "__text");
    buf_write_segname(&out, "__TEXT");
    buf_write64(&out, text_vmaddr + code_offset);
    buf_write64(&out, code.len);
    buf_write32(&out, code_offset);
    buf_write32(&out, 2);
    buf_write32(&out, 0);
    buf_write32(&out, 0);
    buf_write32(&out, 0x80000400);
    buf_write32(&out, 0);
    buf_write32(&out, 0);
    buf_write32(&out, 0);

    /* ---- LC_SEGMENT_64: __LINKEDIT ---- */
    buf_write32(&out, LC_SEGMENT_64);
    buf_write32(&out, sz_seg_nosect);
    buf_write_segname(&out, "__LINKEDIT");
    buf_write64(&out, linkedit_vmaddr);
    buf_write64(&out, PAGE_SIZE);
    buf_write64(&out, linkedit_file_offset);
    buf_write64(&out, linkedit_total);
    buf_write32(&out, VM_PROT_READ);
    buf_write32(&out, VM_PROT_READ);
    buf_write32(&out, 0);
    buf_write32(&out, 0);

    /* ---- LC_LOAD_DYLINKER ---- */
    buf_write32(&out, LC_LOAD_DYLINKER);
    buf_write32(&out, sz_dylinker);
    buf_write32(&out, 12);
    {
        char path[20];
        memset(path, 0, 20);
        strcpy(path, "/usr/lib/dyld");
        buf_write(&out, path, 20);
    }

    /* ---- LC_MAIN ---- */
    buf_write32(&out, LC_MAIN);
    buf_write32(&out, sz_main);
    buf_write64(&out, code_offset);
    buf_write64(&out, 0);

    /* ---- LC_BUILD_VERSION ---- */
    buf_write32(&out, LC_BUILD_VERSION);
    buf_write32(&out, sz_build_ver);
    buf_write32(&out, PLATFORM_MACOS);
    buf_write32(&out, 0x000E0000);  /* minos: macOS 14.0 */
    buf_write32(&out, 0);           /* sdk: 0 */
    buf_write32(&out, 0);           /* ntools: 0 */

    /* ---- LC_LOAD_DYLIB (libSystem) ---- */
    buf_write32(&out, LC_LOAD_DYLIB);
    buf_write32(&out, sz_load_dylib);
    buf_write32(&out, 24);  /* offset to name */
    buf_write32(&out, 2);   /* timestamp */
    buf_write32(&out, 0x05540000); /* current_version */
    buf_write32(&out, 0x00010000); /* compat_version 1.0.0 */
    {
        char name[32];
        memset(name, 0, 32);
        strcpy(name, "/usr/lib/libSystem.B.dylib");
        buf_write(&out, name, 32);
    }

    /* ---- LC_DYLD_CHAINED_FIXUPS ---- */
    buf_write32(&out, LC_DYLD_CHAINED_FIXUPS);
    buf_write32(&out, sz_chained);
    buf_write32(&out, chained_fixups_off);
    buf_write32(&out, chained_fixups_size);

    /* ---- LC_DYLD_EXPORTS_TRIE ---- */
    buf_write32(&out, LC_DYLD_EXPORTS_TRIE);
    buf_write32(&out, sz_exports);
    buf_write32(&out, exports_trie_off);
    buf_write32(&out, exports_trie_size);

    /* ---- LC_SYMTAB ---- */
    buf_write32(&out, LC_SYMTAB);
    buf_write32(&out, sz_symtab);
    buf_write32(&out, 0);
    buf_write32(&out, 0);
    buf_write32(&out, 0);
    buf_write32(&out, 0);

    /* ---- LC_DYSYMTAB ---- */
    buf_write32(&out, LC_DYSYMTAB);
    buf_write32(&out, sz_dysymtab);
    for (int j = 0; j < 18; j++)
        buf_write32(&out, 0);

    /* Pad to code_offset (extra room for codesign to add LC_CODE_SIGNATURE) */
    buf_pad_to(&out, code_offset);

    /* Write code + string data */
    buf_write(&out, code.data, code.len);

    /* Pad to PAGE_SIZE (end of __TEXT segment) */
    buf_pad_to(&out, PAGE_SIZE);

    /* Write __LINKEDIT content: chained fixups */
    /* dyld_chained_fixups_header (28 bytes) */
    buf_write32(&out, 0);          /* fixups_version */
    buf_write32(&out, 32);         /* starts_offset */
    buf_write32(&out, 48);         /* imports_offset */
    buf_write32(&out, 48);         /* symbols_offset */
    buf_write32(&out, 0);          /* imports_count */
    buf_write32(&out, 1);          /* imports_format (DYLD_CHAINED_IMPORT) */
    buf_write32(&out, 0);          /* symbols_format */

    /* Padding to offset 32 */
    buf_write32(&out, 0);

    /* dyld_chained_starts_in_image at offset 32 */
    buf_write32(&out, 3);          /* seg_count (PAGEZERO, TEXT, LINKEDIT) */
    buf_write32(&out, 0);          /* seg_info_offset[0] = 0 (no fixups) */
    buf_write32(&out, 0);          /* seg_info_offset[1] = 0 */
    buf_write32(&out, 0);          /* seg_info_offset[2] = 0 */

    /* Exports trie (minimal: empty root node) */
    {
        uint8_t trie[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        buf_write(&out, trie, 8);
    }

    /* Write to file */
    FILE *f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s' for writing\n", output_path);
        free(code.data); free(strings.data);
        free(str_offsets); free(str_lengths);
        free(out.data);
        return 1;
    }

    fwrite(out.data, 1, out.len, f);
    fclose(f);
    chmod(output_path, 0755);

    /* Ad-hoc sign the binary (required on Apple Silicon) */
    char sign_cmd[2048];
    snprintf(sign_cmd, sizeof(sign_cmd),
             "codesign --force --sign - '%s' 2>/dev/null", output_path);
    system(sign_cmd);

    free(code.data);
    free(strings.data);
    free(str_offsets);
    free(str_lengths);
    free(out.data);

    return 0;
}
