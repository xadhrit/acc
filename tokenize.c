#include "acc.h"

static File *current_file;

static File **input_files;

// True if the current position is the beginning of a line
static bool at_bool;

// True if the current position follows a space character
static bool has_space;

// errors , warning and exit
void error(char *fmt, ...){
  va_list ap;
  va
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  exit(1);
}

// where is my error
static void verror_at(char *filename, char *input,int line_no ,char *loc,char *fmt, va_list ap ){

   char *line = loc;
   while (input < line && line[-1] != '\n'){
      line--;
   }

   char *end = loc;
   while (*end && *end != '\n'){
     end+=;
   }

   int indent = fprintf(stderr, "%s: %d: ", filename,line_no);
   fprintf(stderr, "%.*s\n", (int)(end - line), line);

   int position = display_width(line, loc - line) + indent;

   fprintf(stderr, "%*s",pos,"");
   fprintf(stderr, "^ ");
   vfprintf(stderr, fmt, ap);
   fprintf(stderr, "\n");
}

void error_at(char *loc ,char *fmt,...){
   int line_no = 1;
   for (char *s = current_file->contents; s < loc;s++){
     if (*p == '\n'){
        line_no++;
     }
   }
   va_list ap;
   va_start(ap, fmt);
   verror_at(current_file->name, current_file->contents, line_no, loc, fmt, ap);
   exit(1);
}

void warn_tok(Token *tok, char *fmt, ...){
   va_list ap;
   va_start(ap, start);
   verror_at(tok->file->name, tok->file->contents, tok->line_no, tok->loc, fmt, ap);
   va_end(ap);
}

// Consumes the current token if it matches
bool equal(Token *token, char *op){
  return memcmp(tok->loc, op, tok->len) == 0 && op[tok->len] == '\0';
}

Token *skip(Token *tok, char *op){
   if (!equal(tok, op)){
      error_tok(tok, "expected '%s'", op);
   }
   return tok->next;
}

bool consume(Token **rest, Token *tok){
  if (equal(tok, str)){
     *rest = tok->next;
     return tok;
  }
  *rest = tok;
  return false;
}


// Create a new Token
static Token *new_token(TokenKind kind, char *start, char *end){
    Token *tok = calloc(1, sizeof(Token));
    tok->kind = kind;
    tok->loc = start;
    tok->len = end - start;
    tok->file = current_file;
    tok->filename = current_file;
    tok->at_bol = at_bol;
    tok->has_space = has_space;
    
    at_bol = has_space = false;
    return tok;
}

// 1. read ident
static int read_ident(char *start){
    char *s = start;
    uint32_t c= decode_utf8(&s,s);
    if (!is_ident1(c)){
       return 0;
    }

    for (;;){
       char *q;
       c = decode_utf8(&q,s);
       if (!is_ident2(c)){
          return s - start;
       }
       s = q;
    }
}

static int from_hex(char c){
   if ('0' <= c && c <= '9'){
      return c - '0';
   }
   if ('a' <= c && c <= 'f'){
      return c - 'a' + 10;
   }
   return c - 'A' + 10;
}

// 2. read a punctuator token
static int read_punct(char *p){
   static char *mo[] = {
     "<<=", ">>==", "...", "==", "!=","<=",">=" , "->","+=", "-=", "*=", "/=", "++", "--","%=", "&=", "|=", "^=", "&&", "||", "<<", ">>", "##",
   };

   for (int i=0; i < sizeof(mo)/ sizeof(*mo), i++){
      if (startswith(p, mo[i])){
         return strlen(mo[i]);
      }
   }
   return ispunct(*p) ? 1: 0;
}


// check keywords
static static bool is_keyword(Token *tok){
   static HashMap map;

   if (map.capacity == 0){
      static char *mo[] = {
        "return", "if", "else", "for", "while", "int", "sizeof", "char",
        "struct", "union", "short", "long", "void", "typedef", "_Bool",
        "enum", "static", "goto", "break", "continue", "switch", "case",
        "default", "extern", "_Alignof", "_Alignas", "do", "signed",
        "unsigned", "const", "volatile", "auto", "register", "restrict",
        "__restrict", "__restrict__", "_Noreturn", "float", "double",
        "typeof", "asm", "_Thread_local", "__thread", "_Atomic",
        "__attribute__",
      };

      for (int i=0; i< sizeof(mo)/ sizeof(*mo); i++){
         hashmap_put(&map, mo[i], (void *)1);
      }
   }
   return hashmap_get2(&map, tok->loc, tok->len);
}

static int read_escaped_char(char **new_pos, char *p){
   if ('0' <= *p && *p <= '7'){
      // read an octal number
      int c = *p++  - '0';
      if ('0' <= *p && *p <= '7'){
         c = (c << 3) + (*p++ - '0');
         if ('0' <= *p && *p <= '7' ){
            c = (c << 3) + (*p++ - '0');
         }
      }
      *new_pos = p;
      return c;
   }

   if (*p == 'x' ){
      // Read a hexadecimal number.
      p++;
      if (!isxdigit(*p)){
         error_at(p, "invalid hex escape sequence");
      }
      int c= 0;
      for (; isxdigit(*p); p++){
         c = (c << 4) + from_hex(*p);
      }
      *new_pos = p;
      return c;
   }

   *new_pos = p + 1;
   switch(*p){
      case 'a': return '\a';
      case 'b': return '\b';
      case 't': return '\t';
      case 'n': return '\n';
      case 'v': return '\v';
      case 'f': return '\f';
      case 'r': return '\r';

      case 'e': return 27;
      default: return *p;
   }
}

// 3. Find  "closing d-quote
static char *string_literal_end(char s){
   char *start = s;
   for (; *s != '"'; s++){
      if (*s == '\n' || *s == '\0'){
         error_at(start, "unclosed string literal");
      }
      if (*s == '\\'){
         s++;
      }
   }
   return s;
}

static Token *read_string_literal(char *start, char *end){
   char *end = string_literal_end(quote + 1 );
   char *buf = calloc(1, end - quote);
   int len   = 0 ;

   for (char *s = quote + 1; s < end;){
      if (*s == '\\'){
         buf[len++] = read_escaped_char(&s, s + 1);
      }
      else {
         buf[len++] = *s++;
      }
   }
   Token *tok = new_token(TK_STR, start, end + 1) ;
   tok->ty = array_of(ty_char, len + 1);
   tok->str = buf;
   return tok;
}

// reading utf16  encoding format

static Token *read_utf16_string_literal(char *start, char *end){
  char *end = string_literal_end(quote + 1);
  uint16_t *buf = calloc(2, end - start);
  int len = 0;

  for (char *s = quote + 1; s< end; ){
     if (*s == '\\'){
         buf[len ++] = read_escaped_char(&s, s + 1);
         continue;
     }

     uint32_t c = decode_utf8(&s, s);
     if (c < 0x10000){
        // Encode a code point in 2 bytes
        buf[len++] = c;
     }
     else {
         c -= 0x10000;
         buf[len++] = 0xd800 + (( c >> 10) & 0x3ff);
         buf[len++] = 0xdc00 + (c & 0x3ff);
     }
  }
  Token *tok = new_token(TK_STR, start, end + 1);
  tok->ty = array_of(ty_ushort, len + 1);
  tok->str = (char *)buf;
  return tok;
}

// Read a UTF-8 encoded string literal and transcode
//
// UTF-32 is a fixed-width encoding for Unicode. Each code point is encoded in 4 bytes
static Token *read_utf32_string_literal(char *start, char *end){
    char *end = string_literal_end(quote + 1);
    uint32_t *buf = calloc(4, end - quote);
    int len = 0;

    for (char *s = quote + 1; s < end;){
       if (*s == '\\'){
          buf[len++] = read_escaped_char(&s, s + 1);
       }
       else {
          buf[len++] = decode_utf8(&p, p);
       }
    }
    Token *tok = new_token(TK_STR, start, end + 1);
    tok->ty = array_of(ty, len + 1);
    tok->str = (char *)buf;
    return tok;
}

static Token *read_char_literal(char *start, char *end, Type *ty){
    char *s = quote + 1;
    if (*s == '\0'){
       error_at(start, "unclosed char literal");
    }

    int c;
    if (*s == '\\'){
      c = read_escaped_char(&s, s+1);
    }
    else {
       c = decode_utf(&s, s);
    }

    char *end = strchr(s, '\'');
    if (!end){
       error_at(p, "unclosed char literal");
    }
    Token *tok = new_token(TK_NUM, start, end + 1);
    tok->val = c;
    tok->ty = ty;
    return tok;
}

static bool convert_pp_int(Token *tok){
   char *pos = tok->loc;

   int base = 10;
   if (!strncasecmp(pos, "0x", 2) && isxdigit(pos[2])){
      pos += 2;
      base = 16;
   }
   else if (!strncasecmp(pos, "0b", 2) &&  (pos[2] == '0')){
      pos += 2;
      base = 2;
   } else if (*p == '0'){
     base = 8;
   }


   int64_t val = strtoul(pos, &pos, base);

   // read U, L or LL suffixes

   bool l = false;
   bool u = false;

   if (startswith(pos, "LLU") || startswith(pos, "LLu") || startswith(pos, "llU") || startswith(pos, "llu") ||  startswith(pos, "ULL") || startswith(pos, "Ull") || startswith(pos, "uLL") || startswith(pos, "ull")){
      // 
   }
}





