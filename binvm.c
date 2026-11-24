/*
 * binvm.c - Binary Virtual Machine (strict 8-bit instruction set)
 *
 * An 8-bit one-hot instruction ISA. Source files are plain text made of
 * runs of exactly 8 bits ('0' / '1'), plus ';'-delimited comments.
 *
 *   MOVE_R 00000001   INC 00000100   OUT 00010000   JMP_F 01000000
 *   MOVE_L 00000010   DEC 00001000   IN  00100000   JMP_B 10000000
 *   NOP    00000000   HALT 11111111
 *
 * Build:  cc -std=c99 -O2 -Wall -Wextra -o binvm binvm.c
 * Usage:  binvm compile <src.txt> [-o out.bin]   (omit -o to validate only)
 *         binvm run <prog.bin> [-n]
 *         binvm validate <src.txt>
 *
 * Fixes vs. the previous version:
 *   1. compile-time comment handling no longer passes NULL as the VM
 *      (the old code dereferenced it -> segfault on the first comment).
 *   2. Any multi-bit / unknown byte pattern is now a hard error instead of
 *      being silently ignored by a `default:` label.
 *   3. ';' comments may contain arbitrary text (letters, punctuation, UTF-8),
 *      not only "0 1 ; space tab newline".
 *   4. Jump targets are resolved once at load time into a match table, so
 *      unmatched jumps are reported before execution and there is no runtime
 *      stack overflow/underflow path.
 *   5. IN checks for EOF instead of reading 0xFF out of thin air.
 *   6. A global step limit turns runaway loops into a clear error.
 *   7. Errors are reported with real source line/column at compile time and
 *      with an instruction index at run time (the old fake "line/column"
 *      based on byte offsets was meaningless).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MEMORY_SIZE      30000
#define MAX_SOURCE_SIZE  (1024 * 1024)
#define MAX_COMMENT_LEN  512
#define MAX_INSNS        MEMORY_SIZE
#define MAX_STEPS        500000000LL

typedef enum {
    ERR_OK = 0,
    ERR_FILE_NOT_FOUND,
    ERR_FILE_TOO_LARGE,
    ERR_OUT_OF_MEMORY,
    ERR_INVALID_CHARACTER,
    ERR_BINARY_NOT_8BIT,
    ERR_INVALID_INSTRUCTION,
    ERR_EMPTY_COMMENT,
    ERR_COMMENT_TOO_LONG,
    ERR_UNCLOSED_COMMENT,
    ERR_MISSING_HALT,
    ERR_TOO_MANY_INSTRUCTIONS,
    ERR_UNMATCHED_JUMP,
    ERR_MEMORY_OVERFLOW,
    ERR_MEMORY_UNDERFLOW,
    ERR_EOF_AT_INPUT,
    ERR_STEP_LIMIT,
    ERR_COUNT
} ErrorCode;

static const char *error_messages[ERR_COUNT] = {
    "No error",
    "File not found",
    "File too large",
    "Out of memory",
    "Invalid character (outside a comment only 0 1 ; space tab newline CR)",
    "Instruction must be exactly 8 bits",
    "Invalid instruction encoding (one-hot bit, 0x00 or 0xFF only)",
    "Empty comment",
    "Comment too long",
    "Unclosed comment",
    "Program must contain a HALT instruction (11111111)",
    "Too many instructions for memory",
    "Unmatched jump instruction",
    "Memory overflow (pointer moved past the right end)",
    "Memory underflow (pointer moved past the left end)",
    "Input exhausted (EOF) while an IN instruction was waiting",
    "Step limit exceeded (runaway loop)",
};

/* Instruction encoding ---------------------------------------------------- */

#define INSTR_NOP    0x00
#define INSTR_MOVE_R 0x01
#define INSTR_MOVE_L 0x02
#define INSTR_INC    0x04
#define INSTR_DEC    0x08
#define INSTR_OUT    0x10
#define INSTR_IN     0x20
#define INSTR_JMP_F  0x40
#define INSTR_JMP_B  0x80
#define INSTR_HALT   0xFF

/* A byte is a valid instruction iff it is 0x00, 0xFF, or exactly one bit set. */
static int is_valid_instruction(unsigned char b)
{
    return b == INSTR_NOP || b == INSTR_HALT || (b != 0 && (b & (b - 1)) == 0);
}

/* Byte buffer ------------------------------------------------------------- */

typedef struct {
    unsigned char *data;
    int len;
    int cap;
} CodeBuf;

static ErrorCode codebuf_push(CodeBuf *b, unsigned char byte)
{
    if (b->len >= MAX_INSNS)
        return ERR_TOO_MANY_INSTRUCTIONS;
    if (b->len + 1 > b->cap) {
        int ncap = b->cap ? b->cap * 2 : 1024;
        unsigned char *n = (unsigned char *)realloc(b->data, (size_t)ncap);
        if (!n) return ERR_OUT_OF_MEMORY;
        b->data = n;
        b->cap = ncap;
    }
    b->data[b->len++] = byte;
    return ERR_OK;
}

/* Jump resolution --------------------------------------------------------- */

/* Fills match[i] with the index of the partner of a jump at i. */
static ErrorCode compute_jumps(const unsigned char *code, int len, int **out)
{
    int *match = (int *)malloc(sizeof(int) * (size_t)(len > 0 ? len : 1));
    int *stack = (int *)malloc(sizeof(int) * (size_t)(len > 0 ? len : 1));
    int sp = -1, i;

    if (!match || !stack) {
        free(match);
        free(stack);
        return ERR_OUT_OF_MEMORY;
    }
    for (i = 0; i < len; i++)
        match[i] = -1;

    for (i = 0; i < len; i++) {
        if (code[i] == INSTR_JMP_F) {
            stack[++sp] = i;
        } else if (code[i] == INSTR_JMP_B) {
            if (sp < 0) {
                free(match);
                free(stack);
                return ERR_UNMATCHED_JUMP;
            }
            match[i] = stack[sp];
            match[stack[sp]] = i;
            sp--;
        }
    }
    if (sp >= 0) {
        free(match);
        free(stack);
        return ERR_UNMATCHED_JUMP;
    }

    free(stack);
    *out = match;
    return ERR_OK;
}

/* Compiler ---------------------------------------------------------------- */

static char *read_whole_file(const char *path, long *out_size)
{
    FILE *f = fopen(path, "rb");
    char *src;
    long size;

    if (!f) return NULL;
    {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        if (size < 0 || size > MAX_SOURCE_SIZE) {
            fclose(f);
            return NULL;
        }
        fseek(f, 0, SEEK_SET);
    }

    src = (char *)malloc((size_t)size + 1);
    if (!src) {
        fclose(f);
        return NULL;
    }
    if (size > 0 && fread(src, 1, (size_t)size, f) != (size_t)size) {
        free(src);
        fclose(f);
        return NULL;
    }
    src[size] = '\0';
    fclose(f);
    *out_size = size;
    return src;
}

/* out_path == NULL means "validate only". */
static ErrorCode compile_file(const char *in_path, const char *out_path)
{
    long size = 0;
    char *src = read_whole_file(in_path, &size);
    CodeBuf code = { NULL, 0, 0 };
    ErrorCode err = ERR_OK;
    int i = 0;
    int line = 1, col = 1;
    int in_comment = 0;
    int comment_line = 0, comment_col = 0, comment_len = 0, comment_has_text = 0;
    int bit_run = 0;
    unsigned char acc = 0;
    int bit_line = 0, bit_col = 0;
    int has_halt = 0;

    if (!src)
        return ERR_FILE_NOT_FOUND;

    while (i < (int)size) {
        char c = src[i];

        if (in_comment) {
            if (c == ';') {
                if (!comment_has_text) {
                    printf("%s: line %d, column %d: empty comment\n",
                           in_path, comment_line, comment_col);
                    err = ERR_EMPTY_COMMENT;
                    goto done;
                }
                if (comment_len > MAX_COMMENT_LEN) {
                    printf("%s: line %d, column %d: comment longer than %d bytes\n",
                           in_path, comment_line, comment_col, MAX_COMMENT_LEN);
                    err = ERR_COMMENT_TOO_LONG;
                    goto done;
                }
                in_comment = 0;
                i++;
                col++;
                continue;
            }
            if (c == '\n') {
                line++;
                col = 1;
                comment_len++;
                i++;
                continue;
            }
            if (c == '\r') {
                i++;
                continue;
            }
            if (!isspace((unsigned char)c))
                comment_has_text = 1;
            comment_len++;
            col++;
            i++;
            continue;
        }

        if (c == '0' || c == '1') {
            if (bit_run == 0) {
                bit_line = line;
                bit_col = col;
                acc = 0;
            }
            acc = (unsigned char)((acc << 1) | (unsigned char)(c == '1'));
            bit_run++;
            if (bit_run > 8) {
                printf("%s: line %d, column %d: instruction must be exactly 8 bits\n",
                       in_path, bit_line, bit_col);
                err = ERR_BINARY_NOT_8BIT;
                goto done;
            }
            i++;
            col++;
            continue;
        }

        /* Any non-bit character terminates a pending bit token. */
        if (bit_run) {
            if (bit_run != 8) {
                printf("%s: line %d, column %d: instruction must be exactly 8 bits, got %d\n",
                       in_path, bit_line, bit_col, bit_run);
                err = ERR_BINARY_NOT_8BIT;
                goto done;
            }
            if (!is_valid_instruction(acc)) {
                printf("%s: line %d, column %d: invalid instruction encoding 0x%02X "
                       "(one-hot bit, 0x00 or 0xFF only)\n",
                       in_path, bit_line, bit_col, (unsigned)acc);
                err = ERR_INVALID_INSTRUCTION;
                goto done;
            }
            err = codebuf_push(&code, acc);
            if (err != ERR_OK)
                goto done;
            if (acc == INSTR_HALT)
                has_halt = 1;
            bit_run = 0;
        }

        if (c == ';') {
            in_comment = 1;
            comment_len = 0;
            comment_has_text = 0;
            comment_line = line;
            comment_col = col;
            i++;
            col++;
        } else if (c == ' ' || c == '\t') {
            i++;
            col++;
        } else if (c == '\n') {
            line++;
            col = 1;
            i++;
        } else if (c == '\r') {
            i++;
        } else {
            printf("%s: line %d, column %d: invalid character 0x%02X "
                   "(outside a comment only 0 1 ; space tab newline CR)\n",
                   in_path, line, col, (unsigned)c);
            err = ERR_INVALID_CHARACTER;
            goto done;
        }
    }

    if (bit_run) {
        if (bit_run != 8) {
            printf("%s: line %d, column %d: instruction must be exactly 8 bits, got %d\n",
                   in_path, bit_line, bit_col, bit_run);
            err = ERR_BINARY_NOT_8BIT;
            goto done;
        }
        if (!is_valid_instruction(acc)) {
            printf("%s: line %d, column %d: invalid instruction encoding 0x%02X\n",
                   in_path, bit_line, bit_col, (unsigned)acc);
            err = ERR_INVALID_INSTRUCTION;
            goto done;
        }
        err = codebuf_push(&code, acc);
        if (err != ERR_OK)
            goto done;
        if (acc == INSTR_HALT)
            has_halt = 1;
        bit_run = 0;
    }

    if (in_comment) {
        printf("%s: line %d, column %d: unclosed comment (started here)\n",
               in_path, comment_line, comment_col);
        err = ERR_UNCLOSED_COMMENT;
        goto done;
    }
    if (!has_halt) {
        printf("%s: program must contain HALT (11111111)\n", in_path);
        err = ERR_MISSING_HALT;
        goto done;
    }

    {
        int *match = NULL;
        int j;
        err = compute_jumps(code.data, code.len, &match);
        if (err != ERR_OK) {
            printf("%s: unmatched jump instruction\n", in_path);
            goto done;
        }
        for (j = 0; j < code.len; j++) {
            if ((code.data[j] == INSTR_JMP_F || code.data[j] == INSTR_JMP_B) && match[j] < 0) {
                printf("%s: unmatched jump at instruction #%d\n", in_path, j);
                free(match);
                err = ERR_UNMATCHED_JUMP;
                goto done;
            }
        }
        free(match);
    }

    if (out_path) {
        FILE *out = fopen(out_path, "wb");
        if (!out) {
            err = ERR_FILE_NOT_FOUND;
            goto done;
        }
        if (code.len > 0 && fwrite(code.data, 1, (size_t)code.len, out) != (size_t)code.len)
            err = ERR_FILE_NOT_FOUND;
        fclose(out);
    }

done:
    free(code.data);
    free(src);
    return err;
}

/* VM ---------------------------------------------------------------------- */

typedef struct {
    unsigned char memory[MEMORY_SIZE];
    int ptr;
    int auto_newline;
    int last_char;
    int last_char_valid;
} VM;

static ErrorCode run_program(const unsigned char *code, int len,
                            const int *match, int auto_newline)
{
    VM *vm = (VM *)malloc(sizeof(VM));
    int ip = 0, insn = 0;
    long long steps = 0;
    ErrorCode err = ERR_OK;

    if (!vm)
        return ERR_OUT_OF_MEMORY;

    memset(vm->memory, 0, sizeof(vm->memory));
    vm->ptr = MEMORY_SIZE / 2;
    vm->auto_newline = auto_newline;
    vm->last_char = 0;
    vm->last_char_valid = 0;

    while (ip < len) {
        unsigned char instr = code[ip];

        if (++steps > MAX_STEPS) {
            err = ERR_STEP_LIMIT;
            goto fail;
        }

        switch (instr) {
        case INSTR_MOVE_R:
            if (vm->ptr >= MEMORY_SIZE - 1) { err = ERR_MEMORY_OVERFLOW; goto fail; }
            vm->ptr++;
            break;
        case INSTR_MOVE_L:
            if (vm->ptr <= 0) { err = ERR_MEMORY_UNDERFLOW; goto fail; }
            vm->ptr--;
            break;
        case INSTR_INC:
            vm->memory[vm->ptr] = (unsigned char)(vm->memory[vm->ptr] + 1);
            break;
        case INSTR_DEC:
            vm->memory[vm->ptr] = (unsigned char)(vm->memory[vm->ptr] - 1);
            break;
        case INSTR_OUT: {
            char ch = (char)vm->memory[vm->ptr];
            putchar(ch);
            vm->last_char = ch;
            vm->last_char_valid = 1;
            break;
        }
        case INSTR_IN: {
            int ch = getchar();
            if (ch == EOF) {
                err = ERR_EOF_AT_INPUT;
                goto fail;
            }
            vm->memory[vm->ptr] = (unsigned char)ch;
            break;
        }
        case INSTR_JMP_F:
            ip = (vm->memory[vm->ptr] == 0) ? match[ip] : ip;
            break;
        case INSTR_JMP_B:
            ip = (vm->memory[vm->ptr] != 0) ? match[ip] : ip;
            break;
        case INSTR_HALT:
            goto finish;
        case INSTR_NOP:
        default:
            break;
        }

        ip++;
        insn++;
        continue;

    fail:
        printf("\nRuntime error at instruction #%d: %s\n", insn, error_messages[err]);
        ip = len;
        goto finish_run;
    }

finish:
    if (vm->auto_newline && vm->last_char_valid && vm->last_char != '\n')
        putchar('\n');

finish_run:
    free(vm);
    return err;
}

/* Loader + runner --------------------------------------------------------- */

static ErrorCode run_binary_file(const char *path, int auto_newline)
{
    long size = 0;
    char *raw = read_whole_file(path, &size);
    unsigned char *code;
    int *match = NULL;
    ErrorCode err;
    int i;

    if (!raw)
        return ERR_FILE_NOT_FOUND;

    code = (unsigned char *)malloc((size_t)(size > 0 ? size : 1));
    if (!code) {
        free(raw);
        return ERR_OUT_OF_MEMORY;
    }
    memcpy(code, raw, (size_t)size);
    free(raw);

    for (i = 0; i < (int)size; i++) {
        if (!is_valid_instruction(code[i])) {
            printf("%s: invalid instruction encoding 0x%02X at offset %d\n",
                   path, (unsigned)code[i], i);
            free(code);
            return ERR_INVALID_INSTRUCTION;
        }
    }

    err = compute_jumps(code, (int)size, &match);
    if (err != ERR_OK) {
        printf("%s: unmatched jump instruction\n", path);
        free(code);
        return err;
    }

    err = run_program(code, (int)size, match, auto_newline);

    free(match);
    free(code);
    return err;
}

/* CLI --------------------------------------------------------------------- */

static void print_usage(void)
{
    printf(
        "BinVM - 8-bit binary virtual machine (strict mode)\n"
        "\n"
        "Usage:\n"
        "  binvm compile <src.txt> [-o <out.bin>]   compile, or validate if -o omitted\n"
        "  binvm run <prog.bin> [-n]                execute a compiled program\n"
        "  binvm validate <src.txt>                 parse and check only\n"
        "\n"
        "Options:\n"
        "  -n    do not emit a trailing newline when the program ends\n"
        "\n"
        "Source rules:\n"
        "  * one instruction = exactly 8 bits, e.g. 00000100 (INC)\n"
        "  * one-hot encodings only: 00000001 00000010 00000100 00001000\n"
        "                            00010000 00100000 01000000 10000000\n"
        "  * 00000000 = NOP, 11111111 = HALT (HALT is required)\n"
        "  * comments: ;any text here;  (may span lines, may be UTF-8)\n"
        "  * outside comments only 0 1 ; spaces tabs newlines are allowed\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "compile") == 0) {
        const char *out = NULL;
        ErrorCode err;

        if (argc < 3) {
            printf("error: missing input file\n");
            return 1;
        }
        if (argc >= 5 && strcmp(argv[3], "-o") == 0)
            out = argv[4];
        else if (argc >= 4 && strcmp(argv[3], "-o") == 0) {
            printf("error: -o requires a filename\n");
            return 1;
        }

        err = compile_file(argv[2], out);
        if (err != ERR_OK) {
            printf("compile failed: %s\n", error_messages[err]);
            return 1;
        }
        if (out)
            printf("compiled: %s -> %s\n", argv[2], out);
        else
            printf("valid: %s\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "validate") == 0) {
        ErrorCode err;

        if (argc < 3) {
            printf("error: missing input file\n");
            return 1;
        }
        err = compile_file(argv[2], NULL);
        if (err != ERR_OK) {
            printf("validation failed: %s\n", error_messages[err]);
            return 1;
        }
        printf("valid: %s\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "run") == 0) {
        int auto_newline = 1;
        ErrorCode err;

        if (argc < 3) {
            printf("error: missing filename\n");
            return 1;
        }
        if (argc >= 4) {
            if (strcmp(argv[3], "-n") == 0)
                auto_newline = 0;
            else {
                printf("error: unknown option %s\n", argv[3]);
                return 1;
            }
        }
        err = run_binary_file(argv[2], auto_newline);
        if (err != ERR_OK)
            return 1;
        return 0;
    }

    printf("error: unknown command '%s'\n", argv[1]);
    print_usage();
    return 1;
}
