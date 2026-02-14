CC = cc
CFLAGS = -Wall -Wextra -std=c11
SRC = src/main.c src/lexer.c src/parser.c src/codegen.c
TARGET = lingua
VSIX = lingua-vscode/lingua-0.1.0.vsix

all: $(TARGET) vscode

$(TARGET): $(SRC) src/lexer.h src/parser.h src/codegen.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

lingua-vscode/node_modules:
	cd lingua-vscode && npm install

vscode: lingua-vscode/node_modules
	cd lingua-vscode && npm run compile

$(VSIX): vscode
	cd lingua-vscode && npm run package

vscode-install: $(VSIX)
	code --install-extension $(VSIX)

clean:
	rm -f $(TARGET)
	rm -rf lingua-vscode/out $(VSIX)

.PHONY: all vscode vscode-install clean
