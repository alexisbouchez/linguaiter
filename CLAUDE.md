# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Lingua is a minimal compiler for the Lingua language, written in C11. It produces native binaries directly (no assembler or linker needed) for Linux x86-64 (ELF) and macOS ARM64 (Mach-O).

## Build Commands

```bash
make              # Build compiler + VS Code extension
make lingua       # Build compiler only (output: ./lingua)
make clean        # Remove build artifacts
make install      # Build everything + install VS Code extension
```

Compiler flags: `-Wall -Wextra -std=c11 -Isrc`

## Running

```bash
./lingua main.lingua              # Build and execute immediately (temp binary)
./lingua build main.lingua -o out # Compile to standalone binary
./lingua completions <bash|zsh|fish> # Generate shell completions
./lingua --help                   # Show usage information
```

## VS Code Extension

The `lingua-vscode/` directory contains an LSP-based VS Code extension (syntax highlighting, diagnostics, completions, hover). Uses npm (not bun) for the extension build:

```bash
make vscode          # Build extension
make vscode-install  # Install .vsix to VS Code
```

## Architecture

The compiler follows a classic pipeline: **source -> lexer -> parser (AST) -> codegen -> native binary**.

- **`src/diagnostic.c/h`** - Diagnostic reporting. Provides `SourceLoc` for position tracking, `diag_emit()` for colored error/warning messages with source-line caret display, and `diag_error_no_loc()` for locationless errors. Errors are fatal (`exit(1)`); warnings continue compilation.
- **`src/lexer.c/h`** - Tokenizer. Produces tokens for identifiers, strings, ints (decimal and hex `0x`), floats, bools, arithmetic ops (`+`, `-`, `*`, `/`, `%`), comparison ops (`==`, `!=`, `>`, `>=`, `<`, `<=`), bitwise ops (`&`, `|`, `^`, `~`, `<<`, `>>`), compound assignment (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`), increment/decrement (`++`, `--`), `.` for member access, `[]` for indexing/slicing, and keywords (`break`, `continue`, `import`, `from`, `pub`). Handles escape sequences (including `\{`, `\}`), `//` line comments, `/* */` block comments, and position tracking for error reporting.
- **`src/parser.c/h`** - Recursive descent parser. Builds an AST with a recursive `Expr` type (tagged union) supporting literals, variable references, binary operations (arithmetic, comparison, logical, bitwise), unary operations (`-`, `~`), member access (`obj.field`), indexing (`s[i]`), slicing (`s[i:j]`), and function calls in expressions (`EXPR_FN_CALL`). Precedence chain: `parse_or` > `parse_and` > `parse_comparison` > `parse_bitor` > `parse_bitxor` > `parse_bitand` > `parse_shift` > `parse_additive` > `parse_multiplicative` > `parse_unary` > `parse_postfix` > `parse_primary`. Includes `parse_interpolated_string()` for `"{expr}"` interpolation, `parse_update_clause()` for for-loop update with `++`/`--`/compound assignment, `try_parse_new_only()` for `new ClassName(...)` on RHS (all other function/method calls use `EXPR_FN_CALL` in expressions). Supports `NODE_BREAK`, `NODE_CONTINUE`, `NODE_IMPORT`, `is_pub` flag, and `parse_scope_depth`/`parse_loop_depth` tracking.
- **`src/codegen.h`** - Public codegen interface. Declares `codegen()` used by `main.c`.
- **`src/codegen/codegen.c`** - Platform-agnostic codegen entry point. Recursively evaluates `Expr` trees at compile time via `eval_expr()`. Supports arithmetic with int/float type promotion, string concatenation with `+` (auto-converts non-string operand), comparison operators, logical operators, bitwise operators (int-only), unary negation and bitwise NOT, string indexing/slicing, member access on objects, and `EXPR_FN_CALL` evaluation (via globals `g_ft`, `g_ct`, `g_prints`). Uses a `SymTable` with parent-chain scoping, `EvalResult` values (including `VAL_OBJECT` with `ObjData`), a `ClassTable` for class definitions (with inheritance field flattening), `eval_new_expr()` for object construction, and `evaluate_method_call()` for method dispatch. `ReturnCtx` tracks `has_return`, `has_break`, and `has_continue` for control flow. Standard library string functions (`len`, `trim`, `contains`, `replace`, `to_upper`, `to_lower`, `starts_with`, `ends_with`, `index_of`, `char_at`, `substr`) live in the `"std/string"` stdlib module — resolved in-compiler (no file), gated behind explicit import via `g_stdlib_imported_flags`. User-defined functions shadow stdlib. Recursion supported with a depth limit of 1000. Dispatches to platform-specific backend for binary emission.
- **`src/codegen/elf_x86_64.c`** - Linux x86-64 backend. Emits ELF binary with direct syscalls (no libc). Base address 0x400000.
- **`src/codegen/macho_arm64.c`** - macOS ARM64 backend. Emits Mach-O binary with full headers, segments, load commands, and symbol table. Page alignment 16384.
- **`src/codegen/codegen_internal.h`** - Shared `Buffer` struct and write utilities (`buf_init`, `buf_write`, `buf_write8/16/32/64`, `buf_pad_to`, `buf_free`).
- **`src/import.c/h`** - Module import system. Resolves import paths (relative with `./` or project-root-relative), caches parsed module ASTs to avoid re-parsing, and detects circular imports via an import stack. `import_init()` sets the project root, `import_resolve()` resolves a path and returns the cached/parsed AST, `import_push_file()`/`import_pop_file()` manage the circular detection stack, `import_cleanup()` frees all cached modules. Uses `DiagContext` save/restore for multi-file diagnostic reporting.
- **`src/main.c`** - CLI entry point. Handles `build`, `completions`, direct execution, and `--help`.

## Lingua Language Specification

### Comments

```
// Line comment (to end of line)
/* Block comment (can span multiple lines) */
```

Unterminated `/* */` block comments are a compile error.

### Types

| Type     | Literal examples             | Description                  |
|----------|------------------------------|------------------------------|
| `string` | `"hello"`, `"line\n"`        | String with escape sequences |
| `int`    | `0`, `42`, `0xFF`, `0x1A`    | Integer (decimal or hex, parsed as `long`) |
| `float`  | `3.14`, `0.5`                | Floating-point (`double`)    |
| `bool`   | `true`, `false`              | Boolean                      |

### Escape Sequences (in string literals)

`\n` (newline), `\t` (tab), `\r` (carriage return), `\\` (backslash), `\"` (double quote), `\0` (null), `\{` (literal open brace), `\}` (literal close brace). Unknown escape sequences are a compile error.

### Expressions

Expressions are evaluated at compile time and follow this precedence (lowest to highest):

| Precedence  | Operators                        | Associativity | Description              |
|-------------|----------------------------------|---------------|--------------------------|
| 1 (lowest)  | `or`                             | Non-chaining  | Logical OR               |
| 2           | `and`                            | Non-chaining  | Logical AND              |
| 3           | `==` `!=` `>` `>=` `<` `<=`     | Non-chaining  | Comparison               |
| 4           | `\|`                             | Left          | Bitwise OR               |
| 5           | `^`                              | Left          | Bitwise XOR              |
| 6           | `&`                              | Left          | Bitwise AND              |
| 7           | `<<`, `>>`                       | Left          | Bitwise shift            |
| 8           | `+`, `-`                         | Left          | Addition, subtraction    |
| 9           | `*`, `/`, `%`                    | Left          | Multiply, divide, modulo |
| 10          | unary `-`, `~`                   | Right         | Negation, bitwise NOT    |
| 11          | `.field`, `[i]`, `[i:j]`, `()`  | Left          | Postfix                  |
| 12 (highest)| literals, `(expr)`, identifiers  | —             | Primary                  |

**Arithmetic rules:**
- `int op int -> int`, `float op float -> float`, `int op float -> float` (automatic promotion)
- `%` is integer-only; using `%` with floats is a compile error
- Division/modulo by zero is a compile error
- `string + <any>` or `<any> + string` performs concatenation (auto-converts the non-string operand)
- Arithmetic on `bool` or `string` (except `+`) is a compile error

**Bitwise rules:**
- `&`, `|`, `^`, `~`, `<<`, `>>` are integer-only; using them with other types is a compile error

**Comparison rules:**
- Both operands must have the same type (or int/float with promotion)
- `int`, `float`, and `string` support all comparison operators (`==`, `!=`, `>`, `>=`, `<`, `<=`)
- For `bool`, only `==` and `!=` are allowed
- Result is always `bool`

**Parenthesized expressions:** `(expr)` for grouping

### Variable Declaration

```
const <name> = <expr>;          # immutable, type inferred
var <name> = <expr>;            # mutable, type inferred
const <name>: <type> = <expr>;  # immutable, type annotated
var <name>: <type> = <expr>;    # mutable, type annotated
```

- `<type>` must be one of: `int`, `float`, `string`, `bool`.
- If a type annotation is present, the value's inferred type must match exactly (no coercion).
- `const` variables cannot be reassigned. `var` variables that are never reassigned produce a warning suggesting `const`.

### Assignment

```
<name> = <expr>;
```

- The variable must have been previously declared with `var`.
- Reassigning a `const` variable is a compile error.
- The assigned value's type must match the variable's declared type (no coercion).

### Compound Assignment

```
<name> += <expr>;
<name> -= <expr>;
<name> *= <expr>;
<name> /= <expr>;
<name> %= <expr>;
<name> &= <expr>;
<name> |= <expr>;
<name> ^= <expr>;
<name> <<= <expr>;
<name> >>= <expr>;
```

- Desugars to `<name> = <name> <op> <expr>` at parse time.
- Same type rules as the corresponding operator.

### Increment / Decrement

```
<name>++;
<name>--;
```

- Desugars to `<name> = <name> + 1` / `<name> = <name> - 1` at parse time.
- Also valid in for-loop update clauses: `for (var i = 0; i < n; i++) ...`

### Print

```
print(<expr>);                    # prints value followed by \n
print(<expr>, newline: false);    # prints value without trailing \n
```

- `<expr>` can be any expression (literal, variable, arithmetic, comparison, function call, etc.).
- Values are resolved at compile time and the result string is embedded in the binary.
- By default, `print` appends a newline (`\n`) after the output.
- Pass `newline: false` to suppress the trailing newline.

### Functions

```
fn <name>(<params>) {
    <body>
}

fn <name>(<params>) -> <return_type> {
    <body>
}

// Shorthand: single return statement without braces
fn <name>(<params>) -> <return_type> return <expr>;
```

- Parameters are typed: `name: type`. Parameters with defaults: `name: type = <expr>`.
- Required parameters must come before parameters with defaults.
- Return type is optional; omitting it makes the function void.
- Functions are evaluated at compile time (like everything else). Recursion is supported with a depth limit of 1000 calls.
- `return <expr>;` returns a value. Assigning a void function's result to a variable is a compile error.
- Function calls work as expressions: `fib(n - 1) + fib(n - 2)` is valid.

**Function calls:**

```
<name>(<args>);
<name>(arg1, arg2, param_name: arg3);
```

- Supports positional and named arguments. Positional arguments must come before named arguments.
- Named arguments use `name: value` syntax. Duplicate named arguments are a compile error.

### For Loop

```
for (var <name> = <expr>; <cond>; <update>) {
    <body>
}

// Braceless: single statement body
for (var <name> = <expr>; <cond>; <update>) <statement>;
```

- C-style for loop with init, condition, and update clauses.
- The init clause declares a loop variable (must use `var`).
- The condition must evaluate to a `bool`.
- The update clause supports: `<name> = <expr>`, `<name>++`, `<name>--`, and compound assignment (`<name> += <expr>`, etc.).
- The loop is unrolled at compile time. A safety limit of 10,000 iterations prevents infinite loops.
- The loop variable is scoped to the for loop and removed after it completes.

### Break and Continue

```
break;
continue;
```

- `break` exits the nearest enclosing for loop immediately.
- `continue` skips the rest of the current iteration and proceeds to the update clause.
- Using `break` or `continue` outside of a for loop is a compile error.

### Conditional Statements

```
if (<condition>) {
    <body>
}

if (<condition>) {
    <body>
} else {
    <body>
}

if (<condition>) {
    <body>
} else if (<condition>) {
    <body>
} else {
    <body>
}

// Braceless: single statement bodies
if (<condition>) <statement>;
if (<condition>) <statement>; else <statement>;
if (<condition>) <statement>; else if (<condition>) <statement>; else <statement>;
```

- Condition must evaluate to `bool`; non-bool condition is a compile error.
- Parentheses are required around the condition (like C/JS).
- `else if` chains are supported (parsed as `else { if ... }` internally).
- No trailing `;` required (like `fn` and `for` declarations).
- Evaluated at compile time: only the taken branch is processed.
- Variables declared inside an if/else body are scoped to that block.

### Match Statement

```
match (<expr>) {
    <pattern> => {
        <body>
    }
    _ => {
        <body>
    }
}

// Braceless: single statement arms
match (<expr>) {
    <pattern> => <statement>;
    _ => <statement>;
}
```

- Parentheses are required around the scrutinee expression.
- Matches the scrutinee expression against each arm's pattern top-to-bottom.
- Patterns are expressions (literals, variables, arithmetic) evaluated and compared with `==`.
- `_` is the wildcard arm (default case); must be last if present.
- The first matching arm's body is executed; remaining arms are skipped.
- Each arm body has its own scope (like `if`/`else` blocks).
- Supports all types: `int`, `float`, `string`, `bool` (same type-comparison rules as `==`).
- No trailing `;` required after the closing `}`.
- Evaluated at compile time (like everything else).

### String Interpolation

```
"Hello, {name}!"
"{a} + {b} = {a + b}"
"literal \{braces\}"
```

- Expressions inside `{...}` in string literals are evaluated and their result is concatenated into the string.
- Any valid expression can appear inside `{}`, including arithmetic, function calls, and variable references.
- Use `\{` and `\}` to include literal braces without interpolation.
- Interpolation is desugared to a chain of `+` (concatenation) operations at parse time.

### String Operations

String functions live in the `std/string` standard library module and must be explicitly imported before use:

```
import { len, trim } from "std/string";
import { len, trim, contains, replace, to_upper, to_lower, starts_with, ends_with, index_of, char_at, substr } from "std/string";
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `len(s)` | `string -> int` | Length of string |
| `trim(s)` | `string -> string` | Remove leading/trailing whitespace |
| `contains(s, sub)` | `string, string -> bool` | Check if `s` contains `sub` |
| `replace(s, old, new)` | `string, string, string -> string` | Replace all occurrences |
| `to_upper(s)` | `string -> string` | Convert to uppercase |
| `to_lower(s)` | `string -> string` | Convert to lowercase |
| `starts_with(s, prefix)` | `string, string -> bool` | Check prefix |
| `ends_with(s, suffix)` | `string, string -> bool` | Check suffix |
| `index_of(s, sub)` | `string, string -> int` | Index of first occurrence (-1 if not found) |
| `char_at(s, i)` | `string, int -> string` | Single character at index |
| `substr(s, start, end)` | `string, int, int -> string` | Substring from start to end (exclusive) |

User-defined functions with the same name as a stdlib function shadow the stdlib version.

**Indexing and slicing:**

```
"hello"[0]      // "h" — single character access
"hello"[1:4]    // "ell" — substring slice (start inclusive, end exclusive)
```

- Negative indices wrap around from the end of the string.
- Out-of-bounds access is a compile error for indexing; slicing clamps to valid range.

### Classes

```
class <Name> {
    <field>: <type>;
    fn <method>(<params>) [-> <return_type>] { <body> }
}

class <Child> extends <Parent> {
    <additional_field>: <type>;
    fn <method>(<params>) [-> <return_type>] { <body> }
}
```

- Classes define a named type with fields and methods.
- Fields are typed: `name: type;`. Supported field types: `int`, `float`, `string`, `bool`.
- Methods use the same syntax as functions (`fn`). Inside methods, class fields are accessible as local variables.
- Single inheritance with `extends`: child class inherits all parent fields and methods.
- Child classes can add new fields and override parent methods.
- Method dispatch walks the inheritance chain (child methods take priority).
- No trailing `;` required after the closing `}`.

**Object construction:**

```
const obj = new ClassName(field: value, ...);
const obj = new ClassName(value1, value2, ...);
```

- `new` creates an instance. Arguments are matched to fields by name or position.
- Positional arguments must come before named arguments.
- All fields must be provided; there are no default field values.
- Field types are checked at construction time.

**Field access:**

```
obj.field          // in expressions (returns the field value)
```

- Dot notation accesses object fields. Chains are supported: expressions like `obj.field` can appear anywhere an expression is expected.

**Field mutation:**

```
obj.field = <expr>;
```

- Requires the variable to be declared with `var` (not `const`).
- The assigned value's type must match the field's declared type.

**Method calls:**

```
const result = obj.method(args);   // capture return value
obj.method(args);                  // standalone (void or ignore result)
print(obj.method(args));           // in print
```

- Method calls support the same positional/named argument syntax as function calls.
- Methods can read and mutate the object's fields; mutations are propagated back to the object.
- Calling an undefined method walks the inheritance chain and errors if not found.

### Import and Pub

```
import { name1, name2 } from "path";
pub fn <name>(<params>) { ... }
pub const <name> = <expr>;
pub var <name> = <expr>;
pub class <Name> { ... }
```

- `import` brings named symbols (functions, classes, constants, variables) from another module into scope.
- The `.lingua` extension is automatically appended to the import path.
- Paths starting with `./` are relative to the importing file's directory. Other paths are relative to the project root.
- `pub` marks a declaration as publicly visible to importers. Only `pub` symbols can be imported; importing a non-public symbol is a compile error.
- Both `import` and `pub` are only valid at the top level (not inside functions, loops, etc.).
- Each module is parsed and evaluated only once (cached by absolute path).
- Circular imports are detected and produce an error with the full import chain.
- Transitive imports are supported: if A imports B which imports C, A gets B's symbols and B gets C's symbols.
- Side effects (e.g., `print`) in imported files are discarded.

### Block Scoping

```
{
    <body>
}
```

- Bare `{ ... }` blocks introduce a new scope.
- Variables declared inside a block are not visible outside it.
- Inner scopes can read and mutate variables from outer scopes.
- Variable shadowing is supported: a block can declare a variable with the same name as an outer variable; the inner one takes precedence within the block.
- Blocks can be nested arbitrarily deep.
- No trailing `;` required after the closing `}`.

### Identifiers

Identifiers start with a letter or `_`, followed by letters, digits, or `_`. The keywords `const`, `var`, `print`, `true`, `false`, `fn`, `return`, `and`, `or`, `for`, `if`, `else`, `match`, `class`, `new`, `extends`, `break`, `continue`, `import`, `from`, and `pub` are reserved.

### Statements

A program is a sequence of statements. Every statement ends with `;` (except function declarations, class declarations, for loops, if/else statements, match statements, and bare blocks). Statement forms: variable declaration, assignment, compound assignment, increment/decrement, field assignment, print, function declaration, class declaration, function call, method call, return, for loop, if/else, match, block, break, continue, and import.

## Memory Model

The compiler currently evaluates everything at compile time — variables don't exist at runtime, only pre-resolved string literals for print statements get embedded in the binary. The following documents the target memory model as the language grows to support runtime behavior.

### Stack

- Every local variable gets a uniform **8-byte slot** on the stack, indexed by `[rbp - 8*(slot+1)]` (x86-64) or `[x29 - 8*(slot+1)]` (ARM64)
- `int` = 64-bit signed integer, `float` = 64-bit double, `bool` = 64-bit (0/1), `string` = 64-bit pointer to a length-prefixed object
- Standard frame pointer based: `push rbp; mov rbp, rsp; sub rsp, N` (x86-64) / `stp x29, x30, [sp, #-N]!; mov x29, sp` (ARM64)

### Strings

- Length-prefixed objects: `{u64 len, u8 data[]}`
- String **literals** live in the read-only data section (not copied to the arena)
- Dynamic strings (future: concatenation) are allocated on the arena
- Stack slot holds a pointer to the start of the struct; load `[ptr]` for length, `[ptr+8]` for data

### Arena (Heap)

- 1 MB region allocated at program startup via `mmap` (anonymous, read/write, no libc)
- **Bump pointer** in a dedicated callee-saved register: `r14` (x86-64), `x27` (ARM64)
- **Arena limit** in another callee-saved register: `r15` (x86-64), `x28` (ARM64)
- Allocation: `ptr = bump; bump += size; if (bump > limit) abort;`
- No individual deallocation — freed all at once on program exit (process teardown)

### Calling Convention

- Follows platform ABI: System V AMD64 (x86-64), AAPCS64 (ARM64)
- Arguments in `rdi, rsi, rdx, rcx, r8, r9` (x86-64) / `x0-x7` (ARM64)
- Return value in `rax` (x86-64) / `x0` (ARM64)

### Register Allocation

- No general register allocator — all variables live on the stack
- Expressions use temporary registers (`rax, rcx, rdx` on x86-64 / `x0-x3` on ARM64)
- Reserved: `rbp/rsp/r14/r15` (x86-64), `x29/sp/x30/x27/x28` (ARM64)

### Implementation Phases (future)

1. **IR layer** — introduce `IRInstr`/`IRProgram` between codegen and backends (transparent pass-through initially)
2. **Stack frames + local variables** — variables exist at runtime as stack slots
3. **Runtime comparisons + logical ops** — emit real `cmp`/`cset` instructions
4. **Function calls** — real `call`/`ret` with argument passing per ABI
5. **Arena allocator** — `mmap` at startup, bump pointer in `r14`/`x27`
6. **Runtime int-to-string** — inline `itoa` for `print(int_var)` without libc

## Keeping CLAUDE.md Up-to-Date

This file must be updated whenever a language feature, CLI command, build target, or architecture component is added or changed. Treat CLAUDE.md as part of the deliverable for any feature work.

## No Test Suite

There is currently no automated test infrastructure. Validation is done by compiling and running `.lingua` files manually.
