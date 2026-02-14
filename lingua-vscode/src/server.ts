import {
  createConnection,
  TextDocuments,
  ProposedFeatures,
  InitializeResult,
  TextDocumentSyncKind,
  Diagnostic,
  DiagnosticSeverity,
  CompletionItem,
  CompletionItemKind,
  InsertTextFormat,
  Hover,
} from "vscode-languageserver/node";
import { TextDocument } from "vscode-languageserver-textdocument";

// --- Lexer (port of src/lexer.c) ---

enum TokenType {
  Ident,
  String,
  LParen,
  RParen,
  Semicolon,
  EOF,
  Error,
}

interface Token {
  type: TokenType;
  text: string;
  offset: number;
  length: number;
}

function tokenize(source: string): Token[] {
  const tokens: Token[] = [];
  let pos = 0;

  function skipWhitespace() {
    while (pos < source.length && /\s/.test(source[pos])) pos++;
  }

  while (true) {
    skipWhitespace();

    if (pos >= source.length) {
      tokens.push({ type: TokenType.EOF, text: "", offset: pos, length: 0 });
      break;
    }

    const c = source[pos];

    if (c === "(") {
      tokens.push({ type: TokenType.LParen, text: "(", offset: pos, length: 1 });
      pos++;
      continue;
    }

    if (c === ")") {
      tokens.push({ type: TokenType.RParen, text: ")", offset: pos, length: 1 });
      pos++;
      continue;
    }

    if (c === ";") {
      tokens.push({ type: TokenType.Semicolon, text: ";", offset: pos, length: 1 });
      pos++;
      continue;
    }

    if (c === '"') {
      const start = pos;
      pos++; // skip opening quote
      while (pos < source.length && source[pos] !== '"') pos++;
      if (pos >= source.length) {
        // Unterminated string
        tokens.push({
          type: TokenType.Error,
          text: source.slice(start),
          offset: start,
          length: pos - start,
        });
        continue;
      }
      pos++; // skip closing quote
      tokens.push({
        type: TokenType.String,
        text: source.slice(start, pos),
        offset: start,
        length: pos - start,
      });
      continue;
    }

    if (/[a-zA-Z_]/.test(c)) {
      const start = pos;
      while (pos < source.length && /[a-zA-Z0-9_]/.test(source[pos])) pos++;
      const text = source.slice(start, pos);
      tokens.push({ type: TokenType.Ident, text, offset: start, length: pos - start });
      continue;
    }

    // Unknown character
    tokens.push({
      type: TokenType.Error,
      text: source[pos],
      offset: pos,
      length: 1,
    });
    pos++;
  }

  return tokens;
}

// --- Validator (port of src/parser.c with error recovery) ---

function validate(doc: TextDocument): Diagnostic[] {
  const text = doc.getText();
  const tokens = tokenize(text);
  const diagnostics: Diagnostic[] = [];
  let i = 0;

  function current(): Token {
    return tokens[i] ?? { type: TokenType.EOF, text: "", offset: text.length, length: 0 };
  }

  function advance(): Token {
    const tok = current();
    if (tok.type !== TokenType.EOF) i++;
    return tok;
  }

  function makeDiag(offset: number, length: number, message: string): Diagnostic {
    const startPos = doc.positionAt(offset);
    const endPos = doc.positionAt(offset + Math.max(length, 1));
    return {
      severity: DiagnosticSeverity.Error,
      range: { start: startPos, end: endPos },
      message,
      source: "lingua",
    };
  }

  // Panic-mode recovery: skip to next semicolon or EOF
  function recover() {
    while (current().type !== TokenType.Semicolon && current().type !== TokenType.EOF) {
      advance();
    }
    if (current().type === TokenType.Semicolon) advance();
  }

  while (current().type !== TokenType.EOF) {
    const tok = advance();

    if (tok.type === TokenType.Error) {
      if (tok.text.startsWith('"')) {
        diagnostics.push(makeDiag(tok.offset, tok.length, "Unterminated string literal"));
      } else {
        diagnostics.push(makeDiag(tok.offset, tok.length, `Unexpected character '${tok.text}'`));
      }
      recover();
      continue;
    }

    if (tok.type !== TokenType.Ident) {
      diagnostics.push(makeDiag(tok.offset, tok.length, `Expected function name, got '${tok.text}'`));
      recover();
      continue;
    }

    // We have an identifier — check it's "print"
    if (tok.text !== "print") {
      diagnostics.push(makeDiag(tok.offset, tok.length, `Unknown function '${tok.text}'`));
      recover();
      continue;
    }

    // Expect '('
    const lp = current();
    if (lp.type !== TokenType.LParen) {
      diagnostics.push(makeDiag(tok.offset + tok.length, 0, "Expected '(' after 'print'"));
      recover();
      continue;
    }
    advance();

    // Expect string literal
    const str = current();
    if (str.type === TokenType.Error && str.text.startsWith('"')) {
      diagnostics.push(makeDiag(str.offset, str.length, "Unterminated string literal"));
      advance();
      recover();
      continue;
    }
    if (str.type !== TokenType.String) {
      diagnostics.push(makeDiag(str.offset, Math.max(str.length, 1), "Expected string literal"));
      recover();
      continue;
    }
    advance();

    // Expect ')'
    const rp = current();
    if (rp.type !== TokenType.RParen) {
      diagnostics.push(makeDiag(rp.offset, Math.max(rp.length, 1), "Expected ')' after string"));
      recover();
      continue;
    }
    advance();

    // Expect ';'
    const semi = current();
    if (semi.type !== TokenType.Semicolon) {
      diagnostics.push(makeDiag(rp.offset + rp.length, 0, "Expected ';' after ')'"));
      recover();
      continue;
    }
    advance();
  }

  return diagnostics;
}

// --- LSP Server setup ---

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

connection.onInitialize((): InitializeResult => {
  return {
    capabilities: {
      textDocumentSync: TextDocumentSyncKind.Incremental,
      completionProvider: { triggerCharacters: ["p"] },
      hoverProvider: true,
    },
  };
});

documents.onDidChangeContent((change) => {
  const diagnostics = validate(change.document);
  connection.sendDiagnostics({ uri: change.document.uri, diagnostics });
});

connection.onCompletion((): CompletionItem[] => {
  return [
    {
      label: "print",
      kind: CompletionItemKind.Function,
      detail: "Print a string to stdout",
      insertText: 'print("$1");',
      insertTextFormat: InsertTextFormat.Snippet,
    },
  ];
});

connection.onHover((params): Hover | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;

  const offset = doc.offsetAt(params.position);
  const text = doc.getText();

  // Find the word under cursor
  let start = offset;
  let end = offset;
  while (start > 0 && /[a-zA-Z_]/.test(text[start - 1])) start--;
  while (end < text.length && /[a-zA-Z0-9_]/.test(text[end])) end++;
  const word = text.slice(start, end);

  if (word === "print") {
    return {
      contents: {
        kind: "markdown",
        value: "```lingua\nprint(message)\n```\nPrints a string to standard output followed by a newline.",
      },
    };
  }

  return null;
});

documents.listen(connection);
connection.listen();
