# BinVM

A tiny 8-bit binary virtual machine with a one-hot instruction set. Source is plain
text: every instruction is exactly 8 bits (`0` / `1`), comments are `;`-delimited.

No assembler, no mnemonics — you literally write machine code.

```text
; print a single 'A' ;
00000100 00000100 00000100 00000100 00000100   ; INC x5 -> 5
...
00010000                                       ; OUT
11111111                                       ; HALT
```

## Instruction set

Each opcode occupies exactly one bit of a byte, so any valid instruction is either
`0x00`, `0xFF`, or one-hot.

| binary     | hex  | mnemonic | meaning                                    |
|------------|------|----------|--------------------------------------------|
| `00000000` | 0x00 | NOP      | do nothing                                 |
| `00000001` | 0x01 | MOVE_R   | `ptr++`                                    |
| `00000010` | 0x02 | MOVE_L   | `ptr--`                                    |
| `00000100` | 0x04 | INC      | `mem[ptr]++` (wraps at 255)                |
| `00001000` | 0x08 | DEC      | `mem[ptr]--` (wraps at 0)                  |
| `00010000` | 0x10 | OUT      | `putchar(mem[ptr])`                        |
| `00100000` | 0x20 | IN       | `mem[ptr] = getchar()`, hard error on EOF  |
| `01000000` | 0x40 | JMP_F    | if `mem[ptr] == 0`, jump past matching `JMP_B` |
| `10000000` | 0x80 | JMP_B    | if `mem[ptr] != 0`, jump back to matching `JMP_F` |
| `11111111` | 0xFF | HALT     | stop                                       |

`JMP_F` / `JMP_B` pair up like Brainfuck's `[` / `]`. Pairs are resolved once at load
time; an unmatched jump is an error, never a runtime stack overflow.

## Source syntax

- An instruction is exactly 8 consecutive bit characters. Whitespace only separates
  tokens, so `00000100` and `0000 0100` are different: the second is an error.
- Comments start and end with `;` and may contain any text, including letters, CJK
  and UTF-8 punctuation. They may span multiple lines. Empty or whitespace-only
  comments are rejected.
- Outside comments the only legal characters are `0 1 ; space tab newline CR`.
  Anything else is reported with line and column.
- Exactly one thing is mandatory: a program must contain `HALT` (`11111111`).
- Limits: 30,000 cells of memory, 30,000 instructions, 512 bytes per comment,
  1 MB source file, 500,000,000 executed instructions per run.

## Memory model

- 30,000 cells, each one byte, all zero at start; `INC` / `DEC` wrap instead of trap.
- The data pointer starts in the middle (cell 15,000) so `MOVE_L` has room.
- Moving past either end is a hard error (`memory overflow` / `memory underflow`).

## Build

```bash
make            # build ./binvm (CC, CFLAGS are overridable)
make test       # compile examples + 11 smoke tests, exit 1 on failure
make install    # install to $(PREFIX)/bin, default /usr/local
make clean
```

Or without make: `cc -std=c99 -O2 -Wall -Wextra -Wpedantic -o binvm binvm.c`.
Single file, no dependencies beyond libc. CI runs both gcc and clang
(`.github/workflows/ci.yml`).

## Usage

```bash
binvm compile hello.txt -o hello.bin   # assemble text source to a byte image
binvm compile hello.txt                # no -o: parse and validate only
binvm validate hello.txt               # same thing, named for its purpose
binvm run hello.bin                    # execute
binvm run hello.bin -n                 # suppress the automatic trailing newline
```

`run` prints a trailing newline when the program ends, unless the last output
character was already `\n` or `-n` is given.

## Example: loop three times and print `A`

```text
; counter in c0, character in c1 ;
00000100 00000100 00000100      ; c0 = 3
00000001                        ; -> c1
00000100 x65                    ; c1 = 'A'
00000010                        ; -> c0
01000000                        ; while c0 != 0 {
00000001 00010000 00000010      ;   print c1
00001000                        ;   c0--
10000000                        ; }
11111111                        ; HALT
```

Output: `AAA`

(The real file spells the 65 `INC`s out one per line; the `x65` shorthand above is
only for readability.)

## Errors

Compile-time errors name the file, line and column:

- `instruction must be exactly 8 bits`
- `invalid instruction encoding 0x03 (one-hot bit, 0x00 or 0xFF only)`
- `invalid character 0x68 (outside a comment only 0 1 ; space tab newline CR)`
- `empty comment` / `comment too long` / `unclosed comment`
- `program must contain HALT (11111111)`
- `unmatched jump instruction`
- `too many instructions for memory`

Run-time errors name the instruction index (offset in the byte image):

- `memory overflow` / `memory underflow`
- `input exhausted (EOF) while an IN instruction was waiting`
- `step limit exceeded (runaway loop)` — 500M instructions, ~1s on a modern CPU

Exit status is `0` on success, `1` on any error.

## What changed from the first version

The original `binvm.c` had a few things that made it unusable in practice:

1. **Comment parsing dereferenced a NULL VM.** `compile_to_binary` called
   `parse_comment(NULL, ...)`, and the function then wrote `vm->comment_start_line`
   and `vm->comment_start_col` through that NULL pointer. Any comment containing
   only legal characters (e.g. `; 0101 ;`) crashed the compiler with SIGSEGV.
   Reproduced on the old build: exit code 139.
   Comments also could not contain letters, because the per-line character check
   ran before comment parsing — so a comment could only be made of `0`, `1` and
   spaces, which defeats the point of comments.
2. **Invalid encodings were silently executed.** A byte like `0x03`
   (`MOVE_R|MOVE_L`) fell into `default: break` and was ignored, so corrupt or
   hand-made binaries kept running with nonsense semantics. It is now a hard error
   at both compile and load time.
3. **Jump handling used a runtime stack.** Nesting was pushed on `JMP_F` and popped
   on `JMP_B`, so a malformed binary surfaced as `stack overflow` mid-run rather
   than as a clear "unmatched jump". Targets are now resolved into a match table
   before execution.
4. **`IN` ignored EOF.** `mem[ptr] = getchar()` turned EOF into a byte value and
   kept going. It now fails with a clear error.
5. **Line/column reporting was fake.** `vm->line` / `vm->col` were incremented per
   byte, and `instr == '\n'` can never be true in bytecode, so runtime "line 3
   column 7" pointed nowhere. Compile errors now carry real source coordinates;
   runtime errors carry an instruction index.
6. **`if (code[i] > 0xFF)` was dead code** for an `unsigned char`.
7. Added a step limit so runaway loops terminate with a diagnostic instead of
   hanging the terminal.
