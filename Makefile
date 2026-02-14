CC = cc
CFLAGS = -Wall -Wextra -std=c11
SRC = src/main.c src/lexer.c src/parser.c src/codegen.c
TARGET = lingua

$(TARGET): $(SRC) src/lexer.h src/parser.h src/codegen.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: clean
