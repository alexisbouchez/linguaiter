# lingua

A minimal compiler for the Lingua language, targeting ARM64 macOS native binaries.

## Build

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
let greeting = "Hello, world!";
print(greeting);
print("Hi!");
```

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
cd lingua-vscode
npm run package
code --install-extension lingua-0.1.0.vsix
```
