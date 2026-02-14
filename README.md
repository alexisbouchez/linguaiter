# lingua

A minimal compiler for the Lingua language, targeting ARM64 macOS native binaries.

## Build & Install

```bash
make install
```

This builds the compiler, packages the VS Code extension, and installs it. To just build without installing the extension:

```bash
make
```

## Usage

Run a `.lingua` file directly:

```bash
./lingua main.lingua
```

Or compile to a standalone binary:

```bash
./lingua build main.lingua -o main
./main
```

## Language

```lingua
let greeting = "Hello, world!\n";
print(greeting);
print("Hi!\n");
```

Supported escape sequences: `\n`, `\t`, `\r`, `\\`, `\"`, `\0`

## Shell Completions

```bash
# bash
eval "$(lingua completions bash)"

# zsh
eval "$(lingua completions zsh)"

# fish
lingua completions fish | source
```

## VS Code Extension

The `lingua-vscode/` directory contains a VS Code extension with syntax highlighting, diagnostics, completions, and hover support.

```bash
make install
```

Or build and install separately:

```bash
make vscode
make vscode-install
```
