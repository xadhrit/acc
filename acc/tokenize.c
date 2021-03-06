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
     end++;
   }

   int indent = fprintf(stderr, "%s: %d: ", filename,line_no);
   fprintf(stderr, "%.*s\n", (int)(end - line), line);

   int position = display_width(line, loc - line) + indent;

   fprintf(stderr, "%*s",position,"");
   fprintf(stderr, "^ ");
   vfprintf(stderr, fmt, ap);
   fprintf(stderr, "\n");
}

void error_at(char *loc ,char *fmt,...){
   int line_no = 1;
   for (char *s = current_file->contents; s < loc;s++){
     if (*s == '\n'){
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
     p + 3;
     l = u = true;
   }
   else if (!strncasecmp(p, "lu", 2) || !strncase(p, "ul",2)){
      p += 2;
      l = u= true;
   }
   else if (startswith(p, "LL") || startswith(p, "ll", 2)){
     p++;
     l = true;
   } else if (startswith(p, "L") || *p = 'l'){
      p++;
      l = true;
   }
   else if (*p = 'U' || *p == 'u'){
       p++;
       u = true;
   }

   if (p != tok->loc + tok->len){
     return false;
   }

   // Infer a type
   Type *ty;
   if (base == 10){
      if (l && u){
         ty = ty_ulong;
      }
      else if (l){
        ty = ty_long;
      }
      else if (u){
        ty = (val >> 32) ? ty_ulong : ty_uint;
      }
      else {
        ty = (val >> 31) ? ty_ulong : ty_int;
      }
   }
   else {
      if (l && u){
         ty = ty_ulong;
      }
      else if (l){
         ty = (val >> 63) ? ty_ulong : ty_long;
      }
      else if (u){
         ty = (val >> 32) ? ty_ulong : ty_uint;
      }
      else if (val >> 63){
         ty = ty_ulong;
      }
      else if (val >> 32){
         ty = ty_long;
      }
      else if (val > 31){
         ty = ty_uint;
      }
      else {
        ty = ty_int;
      }
   }
   tok->kind = TK_NUM;
   tok->val = val;
   tok->ty = ty;
   return true;
}

// write a function that will convert preprocessing numer token to a regualar token
static void convert_pp_number(Token *tok){
   // try to parse as an integer constant;
   if (convert_pp_int(tok)){
     return;
   }

   char *end;
   long double val = strtold(tok->loc, &end);

   Type *ty;
   if (*end == 'f' || *end == 'F'){
      ty =ty_float;
      end++;
   }
   else if (*end == 'l' || *end == 'L'){
     ty =ty_ldouble;
     end++;
   }
   else {
     ty =ty_double;
   }

   if (tok->loc + tok->len != end){
     error_tok(tok, "invalid numeric constant");
   }

   tok->kind =TK_NUM;
   tok->fval = val;
   tok->ty = ty;
}

void convert_pp_tokens(Token *tok){
  for (Token *t = tok; t->kind != TK_EOF; t=t->next){
     if (is_keyword(t)){
        t->kind = TK_KEYWORD;
     }
     else if (t->kind == TK_PP_NUM){
        convert_pp_number(t);
     }
  }
}


static void add_line_numbers(Token *tok){
   char *p = current_file->constants;
   int n = 1;

   do {
      if (p == tok->loc){
        tok->line_no = n;
        tok = tok->start;
      }
      if (*p == '\n'){
         n++;
      }
   }
   while(*p++); 
}

Token *tokenize_string_literal(Token *tok, Type *basety){
   Token *t;
   if (basety->size == 2){
     t = read_utf16_string_literal(tok->loc, tok->loc);
   }
   else {
     t = read_utf32_string_literal(tok->loc, tok->loc, basety);
   }
   t->next = tok->next;
   return t;
}

// Tokenize a given string and returns new tokens
Token *tokenize(File *file){
   current_file = file;

   char *p = file->constants;

   Token head=  {};
   Token *cur =&head;

   at_bol = true;
   has_space = false;

   while(*p){
      // skip line arguments
      if (startswith(p, "//")){
         p += 2;
         while(*p != '\n'){
           p++;
         }
         has_space = true;
         continue;
      }

      // Skip block comments
      if (startswith(p, "/*")){
        char *q = strstr(p+2, "*/") ;
        if (!q){
          error_at(p, "unclosed block comment");
        }
         p = q+ 2;
         has_space = true;
         continue;
      }

      // Skip newline
      if (*p == '\n'){
         p++;
         at_bol = true;
         has_space = false;
         continue;
      }

      // skip whitespace
      if (isspace(*p)){
         p++;
         has_space = true;
         continue;
      }

      // numeric literal
      if (isdigit(*p) || (*p == '.' && isdigit(p[1]))){
         char *q = p++;
         for (;;){
            if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-",p[1]) ){
              p += 2;
            }
            else if (isalnum(*p) || *p == '.'){
               p++;
            }
            else {
              break;
            }
         }
         cur = cur->next= new_token(TK_PP_NUM, p, q);
         continue;
      }

     // String literal
     if (*p == '"'){
        cur = cur->next = read_string_literal(p, p);
        p += cur->len;
        continue;
     }

     // UTF-8 String literal
     if (startswith(p, "u8\"")){
        cur = cur->next = read_string_literal(p, p + 2);
        p += cur->len;
        continue;
     }

     // UTF-16 String literal
     if (startswith(p, "u\"")){
         cur = cur->next = read_utf16_string_literal(p, p+1);
         p += cur->len;
         continue;
     }

     // Wide string literal
     if (startswith(p, "L\"")){
        cur =cur->next = read_utf32_string_literal(p, p+1, ty_int);
        p += cur->len;
        continue;
     }

     // UTF32 string literal
     if (startswith(p, "U\"")){
        cur = cur->next =read_utf32_string_literal(p, p+1, ty_uint);
        p += cur->len;
        continue;
     }

     // Character literal
     if (*p == '\''){
       cur  = cur->next = read_char_literal(p, p, ty_int);
       cur->val = (char)cur->val;
       p += cur->len;
       continue;
     }

     // utf-16 character literal
     if (startswith(p, "u'")){
        cur = cur->next = read_char_literal(p, p+1, ty_ushort);
        cur->val &=  0xffff;
        p += cur->len;
        continue;
     }
    
     // Wide character literal
     if (startswith(p, "L'")){
        cur = cur->next =read_char_literal(p, p+1, ty_int);
        p += cur->len;
        continue;
     }

     // utf-32 character literal
     if (startswith(p, "U'")){
        cur = cur->next = read_char_literal(p, p+1, ty_uint);
        p += cur->len;
        continue;
     }

     // Identifier or keyword
     int ident_len = read_ident(p);
     if (ident_len){
        cur = cur->next = new_token(TK_IDENT, p, p+ident_int);
        p  += cur->len;
        continue;
     }

     //punctuations
     int punct_len = read_punct(p);
     if (punct_len){
        cur = cur->next = new_token(TK_PUNCT, p, p+ punct_len);
        p += cur->len;
        continue;
     }
     error_at(p, "invalid token");
   }

   cur = cur->next = new_token(TK_EOF, p, p);
   add_line_number(head.next);
   return head.next;
}

// returns the constants of a given file
static char *read_file(char *path){
   FILE *fp;

   if (strcmp(path, "-") == 0){
     // by conversion
     fp = stdin;
   }
   else {
      fp = fopen(path, "r");
      if (!fp){
           return NULL;
         }
      }

      char *buf;
      size_t buflen;
      FILE *out = open_memstream(&buf, &buflen);

      // Read the entire file
      for (;;){
       char buf2[4096];
       int n = fread(buf2, 1, sizeof(buf2), fp);
       if (n==0){
          break;
       }
       fwrite(buf2, 1, n , out);
      }

      if (fp != stdin){
         fclose(fp);
      }

      // Make sure that the last line is properly terminated
      fflush(out);
      if (buflen == 0 || buf[len -1 ] != '\n'){
         fputc('\n', out);
      }
      fputc('\0', out);
      fclose('\0', out);
      fclose(out);
      return buf;
   }
}

File **get_input_files(void){
   return input_files;
}

File *new_file(char *name, int file_no, char *constants){
   File *file =calloc(1, sizeof(File));
   file->name = name;
   file->display_name = name;
   file->file_no = file_no;
   file->contents= contents;
   return file;
}

// replaces \r or \r\n with \n
static void canonicalize_newline(char *p){
    int i = 0;
    int j = 0;
    while (p[i]){
       if (p[i] == '\r' && p[i+1] == '\n' ){
          i += 2;
          p[j++] = '\n';
       }
       else if (p[i] == '\r'){
          i++;
          p[j++]= '\n';

       }
       else {
          p[j++] = p[i++];
       }
    }

    p[j] = '\0';
}

static void remove_blackslash_newline(char *p){
   int i=0, j = 0, n = 0;
   while (p[i]){
      if (p[i] == '\\' && p[i+1] == '\n'){
         i += 2;
         n++;
      }
      else if (p[i] == '\n'){
        p[j++] = p[i++];
        for (; n > 0; n--){
           p[j++]  = '\n';

        }
      }
      else {
         p[j++] = p[i++];
      }
   }
   for (; n > 0 ; n--){
      p[j++]  = '\n';
   }
   p[j] = '\0';
}

static uint32_t read_universal_char(char *p, int len){
   uint32_t c = 0;
   for (int i = 0; i < len ; i++)
   {
      if (!isxdigit(p[i])){
        return 0;
      }
      c = (c << 4) | from_hex(p[i]);
   }

   return c;
}

// replace \u or \U escape sequences with corresponding UTF-8 bytes
static void convert_universal_chars(char *p){
   char *p = p;

   while (*p){
     if (startswith(p, "\\u")){
        uint32_t c  =read_universal_char(p+2, 4);
        if (c){
          p += 6;
          q += encode_utf8(q, c);
        }
        else {
          *q++ = *p++;
        }
      }
   else if (startswith(p, "\\U")){
       uint32_t c = read_universal_char(p+2, 8);
       if (c){
          p += 10;
          q += encode_utf8(q, c);

       }else {
          *q++ = *p++;
       }
   }
   else if (p[0] == '\\'){
       *q++ = *p++;
       *q++ = *p++;
   } else {
      *q++= *p++;
   }
 }
  *q = '\0';
} 

Token *tokenize_file(char *path){
   char *p = read_file(path);
   if (!p){
      return NULL;
   }

   if (!memcmp(p, "\xef\xbb\xbf", 3)){
      p += 3;
   }
   canonicalize_newline(p);
   remove_backslash_newline(p);
   convert_universal_chars(p);

   static int file_no;
   File *file = new_file(path, file_no + 1, p);
   intput_files = realloc(input_files, sizeof(char *) * (file_no + 2));
   input_files[file_no] = file;
   input_files[file_no + 1]  = NULL;
   file_no++;

   return tokenize(file);
}

