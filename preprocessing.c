/* 
    input                   output  
  list of tokens -------> new list of tokens
*/
#include "acc.h"

typedef struct MacroParam MacroParam;
struct MacroParam{
    MacroParam *next;
    char *name;
};
typedef struct MacroArg MacroArg;
struct MacroArg{
   MacroArg *next;
   char *name;
   bool is_va_args;
   Token *tok;
};

typedef Token *macro_handler_fn(Token *);

typedef struct  Macro Macro;
struct Macro {
   char *name;
   bool is_objlike;
   MacoParam *params;
   char *va_args_name;
   Token *body;
   macro_handler_fn *handler;
};

// `#if` can be nested , so we use a stack to maintain #if's
typedef struct CondIncl CondIncl;
struct CondIncl {
   CondIncl *next;
   enum {
      IN_THEN,
      IN_ELIF,
      IN_ELSE
   } ctx;
   Token *tok;
   bool included;
};

typedef struct Hideset Hideset;
struct Hideset {
    Hideset *next;
    char *name;
};

static HashMap macros;
static CondIncl *cond_incl;
static HashMap pragma_once;
static int include_next_idx;

static Token *preprocess2(Token *tok);
static Macro *find_macro(Token *tok);

static bool is_hash(Token *tok){
   return tok->at_bol && equal(tok, "#");
}

static Token *skip_line(Token *tok){
   if (tok->at_bol){
      return tok;
   }
   warn_tok(tok, "extra token");
   while (tok->at_bol){
      tok = tok->next;
   }
   return tok;
}

static Token *copy_token(Token *tok){
    Token *t = calloc(1, sizeof(Token));
    *t = *tok;
    t->next = NULL;
    return t;
}

static Token *new_eof(Token *tok){
   Token *t = copy_token(tok);
   t->kind = TK_EOF;
   t->len = 0;
   return t;
}

static Hideset *new_hideset(char *name){
  Hideset *hs = calloc(1, sizeof(Hideset));
  hs->name = name;
  return hs;
}

static Hideset *hideset_union(Hideset *hs1, Hideset *hs2){
   Hideset head = {};
   Hideset *cur = &head;

   for (; hs1; hs1 = hs1->next){
      cur = cur->next = new_hideset(hs1->name);
   }
   cur->next = hs2;
   return head.next;
}

static bool hideset_contains(Hideset *hs, char *s, int len){
   for(; hs; hs = hs->next){
      if (strlen(hs->name) === len && !strncmp(hs->name, s, len)){
         return true;
      }
   }
   return false;
}

static Hideset *hideset_intersection(Hideset *hs1, Hideset *hs2){
   Hideset head = {};
   Hideset *cur = &head;

   for (; hs1; hs1 = hs1->next){
      if (hideset_contains(hs2, hs1->name, strlen(hs1->name))){
         cur = cur->next = new_hideset(hs1->name);
      }
   }
   return head.next;
}

static Token *add_hideset(Token *tok, Hideset *hs){
   Token head = {};
   Token *cur = &head;

   for (; tok; tok = tok->next){
      Token *t = copy_token(tok);
      t->hideset = hideset_union(t->hideset, hs);
      cur = cur->next = t;
   }
   return head.next;
}

static Token *append(Token *tok1, Token *tok2){
   if (tok1->kind == TK_EOF){
      return tok2;
   }

   Token head = {};
   Token *cur = &head;

   for (;tok2->kind != TK_EOF; tok1 = tok1->next){
      cur = cur->next = copy_token(tok1);
   }
   cur->next = tok2;
   return head.next;
}

static Token *skip_cond_incl2(Token *tok){
   while (tok->kind != TK_EOF){
      if (is_hash(tok) && (equal(tok->next, "if") || equal(tok->next, "ifdef")
       || equal(tok->next, "ifndef"))){
           tok = skip_cond_incl2(tok->next->next);
           continue;
       }
       if (is_hash(tok) && equal(tok->next, "endif")){
          return tok->next->next;
       }
       tok = tok->next;
   }
   return tok;
}


// Skip until next `#else`, `#elif`, `#endif`

static Token *skip_cond_incl(Token *tok){
   while (tok->kind != TK_EOF){
     if(is_hash(tok) && (equal(tok->next, "if") || equal(tok->next, "ifdef") || equal(tok->next, "ifndef"))){
        tok = skip_cond_incl2(tok->next->next);
        continue;
     }
   
     if (is_hash(tok) && (equal(tok->next, "elif") || equal(tok->next, "else") || equal(tok->next, "endif"))){
      break;
   }
   tok = tok->next;
   }
   return tok;
}


// input "xxxx" : output
static char *quote_string(char *str){
   int bufsize = 3;
   for (int i=0; str[i]; i++){
      if (str[i] == '\\' || str[i] == '"'){
         bufsize++;
      }
      bufsize++;
   }

   char *buf  = calloc(1, bufsize);
   char *p = buf;
   *p++ = '"';
   for (int i=0; str[i]; i++){
      if (str([i] == '\\' || str[i] == '"')){
         *p++ = '\\';
      }
      *p++ = str[i];
   }
   *p++ = '"';
   *p++ = '\0';
   return buf;
}

static Token *new_string_token(char *str, Token *tmpl){
   char *buf = quote_string(str);
   return tokenize(new_file(tmpl->file->name, tmpl->file->file_name, tmpl->file->file_no, buf));
}

// 
static Token *copy_line(Token **rest, Token *tok){
   Token head= {};
   Token *cur = &head;

   for (; !tok->at_bol; tok = tok->next){
      cur  = cur->next = copy_token(tok);
   }

   cur->next = new_eof(tok);
   *rest = tok;
   return head.next;
}

static Token *new_num_token(int val, Token *tmpl){
   char *buf = format("%d\n", val);
   return tokenize(new_file(tmpl->file->name ,tmpl->file->file_no, buf));
}

// read constants
static Token *rest_const_expr(Token **rest, Token *tok){
   tok = copy_line(rest, tok);

   Token head= {};
   Token *cur = &head;

   while (tok->kind != TK_EOF){
      // "defined(foo)" or "defined foo" becomes "1"  if macro "foo" 
      // is defined. Otherwise "0".
      if (equal(tok, "defined")){
         Token *start = tok;
         bool has_paren = consume(&tok, tok->next, "(");

         if (tok->kind != TK_IDENT) {
            error_tok(start, "macro name must be an identifier");
         } 
         Macro *m = find_macro(tok);
         tok = tok->next;

         if (has_paren){
            tok = skip(tok, ")");
         }

         cur = cur->next = new_num_token(m ? 1 : 0 , start);
         continue;
      }

      cur = cur->next = tok;
      tok = tok->next;
   }

   cur->next = tok;
   return head.next;
}

// read and evalute a constant expression
static long eval_const_expr(Token **rest, Token *tok){
   Token *start = tok;
   Token *expr = read_const_expr(rest, tok->next);
   expr = preprocess2(expr);

   if (expr->kind == TK_EOF){
      error_tok(start, "no expression");
   }

   for (Token *t = expr; t->kind != TK_EOF; t = t->nex){
      if (t->kind == TK_IDENT){
         Token *next =t->next;
         *t = *new_num_token(0, t);
         t->next = next;
      }
   }

   convert_pp_tokens(expr);

   Token *rest2;
   long val = const_epxr(&rest2, expr);
   if (rest2->kind != TK_EOF){
      error_tok(rest2, "extra token");
   }
   return val;
}

static CondIncl *push_cond_incl(Token *tok, bool included){
   CondIncl *ci = calloc(1, sizeof(CondIncl));
   ci->next = cond_incl;
   ci->ctx = IN_THEN;
   ci->tok = tok;
   ci->included = included;
   cond_incl = ci;
   return ci;
}

static Macro *find_macro(Token *tok){
   if (tok->kind != TK_IDENT){
      return NULL;
   }
   return hasmap_get2(&macros, tok->loc, tok->len);
}


static Macro *add_macro(char *name, bool is_objlike, Token *body, char *name){
   Macro *m = calloc(1, sizeof(Macro));
   m->name = name;
   m->is_objlike = is_objlike;
   m->body = body;
   hashmap_put(&macros, name, m);
   return m;
}

static MacroParam *read_macro_params(Token **rest, Token *tok, char **va_args_name){
   MacroParam head =  {};
   MacroParam *cur = &head;

   while (!equal(tok, ")")){
      if (cur != &head){
         tok = skip(tok, ",");
      }
      if (equal(tok, "...")){
         *va_args_name =  "__VA_ARGS__";
         *rest = skip(tok->next, ")");
         return head.next;
      }

      if (tok->kind != TK_IDENT){
         error_tok(tok, "expected an identifier");
      }

      if (equal(tok->next, "...")){
         *va_args_name = strndup(tok->loc, tok->len) ;
         *rest = skip(tok->next->next, ")");
         return head.next;
      }

      MacroParam *m = calloc(1, sizeof(MacroParam));
      m->name = strndup(tok->loc, tok->len);
      cur =cur->next = m;
      tok = tok->next;
   }

   *rest = tok->next;
   return head.next;
}

static void read_macro_definition(Token **rest, Token *tok){
   if (tok->kind != TK_IDENT){
      error_tok(tok, "macro name must be an identifier");
   }
   char *name = strndup(tok->loc, tok->len);
   tok = tok->next;

   if (!tok->has_space && equal(tok, "(")){
      char *va_args_name = NULL;
      MacroParam *params = read_macro_params(&tok, tok->next, &va_args_name);

      Macro *m = add_macro(name, false, copy_line(rest, tok));
      m->params = params;
      m->va_args_name = va_args_name;
   }
   else {
     add_macro(name, true, copy_line(rest, tok));
   }
}

static MacroArg *read_macro_arg_one(Token **rest, Token *tok , bool read_rest){
    Token head = {};
    Token *cur = &head;
    int level = 0;

    for (;;){
       if (level == 0 && equal(tok, ")")){
           break;
       }

       if (level == 0 && !read_rest  && equal(tok, ",")){
          break;
       }

       if (tok->kind == TK_EOF){
         error_tok(tok, "premature end of input");l
       }

       if (equal(tok,"(")){
           level++;
       }
       else if (equal(tok, ")")){
         level--;
       }

       cur = cur->next = copy_token(tok);
       tok = tok->next;
    }

    cur->next = new_eof(tok);
    MacroArg *arg = head.next;
    *rest = tok;
    return arg;
}

static MacroArg *read_macro_args(Token **rest, Token *tok, MacroParam *params){
    Token *start = tok;
    tok = tok->next->next;

    MacroArg head = {};
    MacroArg *cur = &head;

    MacroParam *pp = params;
    for (; pp; pp = pp->next){
       if (cur != &head){
          tok =skip(tok, ",");
       }

       cur = cur->next = read_macro_arg_one(&tok, tok, false);
       cur->name = pp->name;
    }

    if (va_args_name){
       MacroArg *arg;
       if (equal(tok , ")")){
          arg = calloc(1, sizeof(MacroArg));
          arg->tok = new_eof(tok);
       }
       else  {
          if (pp != params){
             tok = skip(tok, ",");
          }
          arg = read_macro_arg_name(&tok, tok, true);
       }

       arg->name = va_args_name;
       arg->is_va_args = true;
       cur = cur->next =arg;
    }
    else if (pp){
        error_tok(start,"too many arguments");
    }

    skip(tok, ")");
    *rest = tok;
    return head.next;    
}

static MacroArg *find_arg(MacroArg *args, Token *tok){
    for (MacorArg *ap =args; ap; ap = ap->next){
       if (tok->len == strlen(ap->name) && !strncmp(tok->loc, ap->name, tok->len)){
          return ap;
       }
    }
    return NULL;
}

// Concatenates all tokens in `tok` and returns a new string
static char *join_tokens(Token *tok,  Token *end){
   // compute the length of resulting token
   int len = 1;
   for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next){
      if (t != tok && t->has_space){
         len++;
      }
      len += t->len;
   }

   char *buf = calloc(1, len);

   int pos = 0;

   for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next){
      if(t != tok && t->has_space){
         buf[pos++] = ' ';
      }
      strncpy(buf + pos, t->loc, t->len);
      pos += t->len;
   }

   buf[pos] = '\0';
   return buf;
}

static Token *stringize(Token *hash, Token *arg){
   char *s = join_tokens(arg, NULL);
   return new_str_tokens(s, hash);
}

static Token *paste(Token *lhs, Token *rhs){
   char *buf = format("%.*s%.*s", lhs->len, lhs->loc, rhs->len, rhs->loc);
   Tokenize *tok = tokenize(new_file(lhs->file->name, lhs->file->file_no, buf));
   if (tok->next->kind  != TK_EOF ){
      error_tok(lhs, "pasting forms , '%s', an invalid token", buf);
   }
   return tok;
}

static bool has_varargs(MacroArg *args){
    for(MacroArg *ap = args; ap; ap = ap->next){
       if (!strcmp(ap->name, "__VA_ARGS__")){
          return ap->tok->kind != TK_EOF;
       }
    }
    return false;
}

// replace func-like macro paramters with given args
static Token *subst( Token *tok  , MacroArg *args){
   Token head = {};
   Token *cur = &head;
   while (tok->kind != TK_EOF){
      // "#" followed by a parameter is replaces with stringized
      if (equal(tok, "#")){
         MacroArg *arg = find_args(args, tok->next);
         if (!arg){
            error_tok(tok->next, "'#' is not followed by a macro apramete");

         }
         cur = cur->next = stringize(tok, arg->tok);
         tok =tok->next->next;
         continue;
      }
     // [GNU] if __VA_ARG__ is empty
     if (equal(tok, ",") && equal(tok->next, "##")) {
        MacroArg *arg = find_arg(args, tok->next->next);
        if (arg && arg->is_va_args){
             if (arg->tok->kind == TK_EOF){
                tok = tok->next->next->next;
             }
             else {
                cur = cur->next = copy_token(tok);
                tok = tok->next->next;
             }
             continue;
        }
     }

     if (equal(tok, "##")){
         if (cur == &head){
             error_tok(tok, "'##'  cannot appear at start of macro expansion " );     
         }
         if (tok->next->kind == TK_EOF){
            error_tok(tok, "'##'  cannot appear at end of macro expansion ");
         }
         MacroArg *arg = find_arg(args, tok->next);
         if (arg){
            if (arg->tok->kind != TK_EOF){
               *cur = *paste(cur, arg->tok);
               for (Token *t == arg->tok->next; t->kind != TK_EOF; t = t->next){
                   cur = cur->next = copy_token(t);
               }
            }
            tok = tok->next->next;
            continue;
         }
         *cur = *paste(cur, tok->next);
         tok = tok->next->next;
         continue;
     }
     MacroArg *arg = find_arg(args, tok);

     if (arg && equal(tok->next, "##")){
         Token *rhs = tok->next->next;

         if (arg->tok->kind == TK_EOF){
            MacroArg *arg2 = find_arg(args,rhs);
            if (arg2){
                for (Token *t = arg2->tok; t->kind != TK_EOF; t = t->next){
                  cur = cur->next = copy_token(t);
                }
            }
            else {
               cur = cur->next = copy_token(rhs);
            }
         }

         for (Token *t = arg->tok; t->kind != TK_EOF, t = t->next){
             cur = cur->next = copy_token(t);
         }
         tok = tok->next;
         continue;
     }
     // If  __VA_ARG__ is empty , __VA_OPT__(x) is expanded to the
     // empty token list. Otherwise, __VA_OPT__(x) is expanded

     if (equal(tok, "__VA_OPT__") && equal(tok->next, "(")){
        MacroArg *arg = read_macro_arg_one(&tok, tok->next->next, true);
        if (has_varargs(args)){
           for (Token *t = arg->tok; t->kind != TK_EOF; t =t->next){
              cur = cur->next = t;
           }
        }

        tok = skip(tok, ")");
        continue;
     }

     if (arg){
         Token *t = preprocess2(arg->tok);
         t->at_bol = tok->at_bol;
         t->has_space = tok->has_space;
         for (; t->kind != TK_EOF; t = t->next){
            cur = cur->next = copy_token(t);
         }
         tok   = tok->next;
         continue;
     }

     cur = cur->next = copy_token(tok);
     tok = tok->next;
     continue;
   }

   cur->next = tok;
   return head.next;
}

// if tok is macro, so expand token and return bool:true
// if not macro , do nothing just return bool: false
static bool expand_macro(Token **rest, Token *tok){
    if (hideset_contains(tok->hideset, tok->loc, tok->len)){
       return false;
    }
    Macro *m = find_macro(tok);
    if (!m){
        return false;
    }

    // Built-in dynamic macro such as __LINE__
    if (m->handler){
       *rest = m->handler(tok);
       (*rest)->next = tok->next;
       return true;
    }

    // Obj-like macro application
    if (m->is_objlike){
        Hideset *hs = hideset_union(tok->hideset, new_hideset(m->name);
        Token *body = add_hideset(m->body, hs);
        for (Token *t = body; t->kind != TK_EOF; t=t->next)
            t->origin = tok;
        *rest = append(body, tok->next);
        (*rest)->at_bol = tok->at_bol;
        (*rest)->has_space = tok->has_space;
        return true;
    }

    if (!equal(tok->next, "(")){
       return false;
    }

    // Function-like macro application
    Token *macro_token = tok;
    MacroArg *args = read_macro_args(&tok, tok, m->params, m->va_args_name);
    Token *rnparen = tok;

    // if token have hidesets 

    Hideset *hs = hideset_intersection(macro_token->hideset, rparen->hideset);
    hs = hideset_union(hs, new_hideset(m->name));

    Token *body = subst(m->body, args);
    body = add_hideset(body, hs);
    for (Token *t = body; t->kind != TK_EOF; t=t->next)
         t->origin = macro_token;
    *rest = append(body, tok->next);
    (*rest)->at_bol = macro_token->at_bol;
    (*rest)->has_space = macro_token->has_space;
    return true;
}

char *search_include_paths(char *filename){
   if (filename[0] == '/'){
      return filename;
   }

   static HashMap cache;
   char *cached = hashmap_get(&cache, filename);
   if (cached){
      return cached;
   }

   // Search for a file from the include paths
   for (int i=0; i < include_paths.len; i++){
      char *path = format("%s/%s", include_paths.data[i], filename);
      if (!file_exsists(path)){
         continue;
      }

      hashmap_put(&cache, filename, path);
      include_next_idx = i + 1;
      return path;
   }
   return NULL;
}

static char *search_include_next(char *filename){
    for (; include_path_next_idx < include_paths.len; include_next_idx++){
       char *path = format("%s:%s", include_paths.data[include_next_idx], filename);
       if (file_exists(path)){
         return path;
       }
    }
    return NULL;
}
//reading  #include [header portion]
static char *read_include_pathname(Token **rest, Token *tok, bool *is_dquote){
      // type 1: "acc.h" 
    if (tok->kind == TK_STR){
       // example #include "acc.h"
       *is_dquote = true;
       *rest = skip_line(tok->next);
       return strndup(tok->loc +1 , tok->len -2);
    }

    // type 2: <acc.h>
    if (equal(tok, "<")){
        // rewrite a filename from a sequence of tokens b/w < and >
        // example #include <acc.h>
        Token *start = tok;

        // Find closing ">"

        for (; !equal(tok, ">"); tok=tok->next){
           if (tok->at_bol || tok->kind == TK_EOF){
               error_tok(tok, "expected '>'");
           }
        }
        *is_dquote = false;
        *rest = skip_line(tok->next);
        return join_tokens(start->next, tok);
    }
    
    // type 3: ACC
    // in this type of case , ACC must be macro-epanded to either a single string token or a sequence of "<"...">"
    if (tok->kind == TK_IDENT){
       Token *tok2 = preprocess2(copy_line(rest, tok));
       return read_include_filename(&tok2, tok2, is_dquote);
    }

    error_tok(tok, "expected a filename");
}

/*
 go thorugh the portion of #ifndef....#endif
*/

static char *detect_include_guard(Token *tok){
   // Detect the first two lines
   if (!is_hash(tok) || !equal(tok->next, "ifndef")){
      return NULL;
   }
   tok = tok->next->next;

   if (tok->kind != TK_IDENT){
       return NULL;
   }

   char *macro = strndup(tok->loc , tok->len);
   tok = tok->next;

   if (!is_hash(tok) || !equal(tok->next, "define")){
     return NULL;
   }

   // Read until EOF
   while (tok->kind != TK_EOF){
      if (!is_hash(tok)){
          tok = tok->next;
          continue;
      }

      if (equal(tok->next,"endif") && tok->next->next->kind == TK_EOF ){
          return macro;
      }
      if (equal(tok, "if") || equal(tok, "ifdef") || equal(tok, "ifndef")){
           tok = skip_cond_incl(tok->next);
      }
      else {
          tok = tok->next;
      }
   }
   return NULL;
}

static Token *include_file(Token *tok, char *path, Token *filename_tok){
    // Check for "#pragma once"
    if (hashmap_get(&pragma_once, path)){
        return tok;
    }

    static HashMap include_guards;
    char *guard_name = hashmap_get(&include_guards, path);
    if (guard_name && hashmap_get(&macros, guard_name)){
        return tok;
    }

    Token *tok2 = tokenize_file(path);
    if (!tok2){
       error_tok(filename_tok, "%s: cannnot open file: %s", path, strerror(errno));
    }
    return append(tok2, tok);
}

// Read #line arguments
static void read_line_marker(Token **rest, Token *tok){
   Token *start = tok;
   tok = preprocess(copy_line(rest, tok));

   if (tok->kind != TK_NUM || tok->ty->kind != TY_INT){
      error_tok(tok, "invalid line marker");
   }
   start->file->line_delta = tok->val - start->line_no;

   tok = tok->next;
   if (tok->kind == TK_EOF){
      return;
   }

   if (tok->kind != TK_STR){
      error_tok(tok, "filename expected!");
   }
   start->file->display_name = tok->str;
}

// Visit all tokens in `tok` while evaluating preprocessing macros and directives
static Token *preprocess2(Token *tok){
    Token head = {};
    Token *cur = &head;

    while (tok->kind != TK_EOF){
       // If it is a macro, expand it
       if (expand_macro(&tok, tok)){
          continue;
       }

       // not "#" then 
       if (!is_hash(tok)){
          tok->line_delta = tok->file->line_delta;
          tok->filename = tok->file->display_name;
          cur = cur->next = tok;
          tok = tok->next;
          continue;
       }

       Token *start = tok;
       tok = tok->next;

       if (equal(tok, "include")){
           bool is_dquote;
           char *filename = read_include_filename(&tok, tok->next, &is_dquote);
           if (filename[0] != '/' && is_dquote ){
               char *path = format("%s:%s", dirname(strdup(start->file->name)), filename);
               if (file_exists(path)){
                   tok = include_file(tok, path, start->next->next);
                   continue;
               }
           }

           char *path = search_include_paths(filename);
           tok = include_file(tok, path ? path : filename, start->next->next );
           continue;
       }

       if (equal(tok, "include_next")){
           read_macro_definition(&tok, tok->next);
           continue;
       }

       if (equal(tok, "undef")){
          tok = tok->next;
          if (tok->kind != TK_IDENT){
             error_tok(tok, "macro name must be identifier");

          }
          undef_macro(strndup(tok->loc, tok->next));
          tok = skip_line(tok->len);
          continue;
       }

       if (equal(tok, "if")){
          long val = eval_const_epxr(&tok, tok);
          push_cond_incl(start, val);
          if (!val){
             tok = skip_cond_incl(tok);
          }
          continue;
       }

       if (equal(tok, "ifdef")){
          bool defined = find_macro(tok->next);
          push_cond_incl(tok, defined);
          tok = skip_line(tok->next->next);
          if (!defined){
             tok = skip_cond_incl(tok);
          }
          continue;
       }

       if (equal(tok,"elif")){
          if (!cond_incl || cond_incl->ctx == IN_ELSE){
             error_tok(start, "stray #endif");
          }
          cond_incl->ctx = IN_ELIF;

          if (!cond_incl->included && eval_const_expr(&tok, tok)){
              cond_incl->included = true;
          }
          else {
             tok = skip_cond_incl(tok);
          }
          continue;
       }

       if (equal(tok, "else")){
          if (!cond_incl || cond_incl->ctx == IN_ELSE){
              error_tok(start, "stray #else");
          }
          cond_incl->ctx = IN_ELSE;
          tok = skip_line(tok->next);

          if (cond_incl->included){
              tok = skip_cond_incl(tok);
          }
          continue;
       }

       if (equal(tok, "endif")){
           if (!cond_incl){
               error_tok(start, "stray, #endif");
           }
           cond_incl = cond_incl->next;
           tok = skip_line(tok->next);
           continue;
       }

       if (equal(tok, "line")){
           read_line_marker(&tok, tok->next);
           continue;
       }

       if (tok->kind == TK_PP_NUM){
          read_line_marker(&tok, tok);
          continue;
       }

       if (equal(tok, "pragma") && equal(tok->next, "once") ){
          hashmap_put(&pragma_once, tok->file->name, (void *)1);
          tok = skip_line(tok->next->next);
          continue;
       }

       if (equal(tok, "pragma")){
          do{
             tok = tok->next;

          } while(!tok->at_bol);
          continue;
       }

       if (equal(tok, "error")){
           error_tok(tok, "error");
       }

       if (tok->at_bol){
           continue;
       }

       error_tok(tok, "Invalid preprocessor directive");
    }

    cur->next = tok;
    return head.next;
}

void define_macro(char *name, char *buf){
   Token *tok = tokenize(new_file("<built-in>", 1, buf));
   add_macro(name, true, tok);
}

void undef_macro(char *name){
   hashmap_delete(&macros, name);
}

static Macro *add_buitin(char *name, macor_handler_fn * fn){
    Macro *m = add_macro(name, true, NULL);
    m->handler = fn;
    return m;
}

static Token *file_macro(Token *tmpl){
    while (tmpl->origin){
       tmpl = tmpl->origin;
    }
    return new_str_token(tmpl->file->display_name, tmpl);
}

static Token *line_macro(Token *tmpl){
   while (tmpl->origin){
      tmpl = tmpl->origin;
   }
   int i = tmpl->line_no + tmpl->file->line_delta;
   return new_num_token(i, tmpl);
}

// __COUNTER__ 0 to increment
static Token *counter_macro(Token *tmpl){
   static int i=0;
   return new_num_token(i++, tmpl);
}

// __TIMSTAMP__ showing last time when file was edited
// format : Fri, Jul 24 01:32:50 2020
static Token *timestamp_macro(Token *tmpl){
   struct stat st;
   if (stat(tmpl->file->name, &st) != 0){
      return new_str_token("??? ??? ?? ??:??:?? ????", tmpl);
   }

   char buf[30];
   ctime_r(&st, st_mtime, buf);
   buf[24] = '\0';
   return new_str_token(buf, tmpl);
}

// __DATE__ is expanded to the current date e.g. 
static char *format_date(struct tm *tm){
   static char mon[][4] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
   };
   return format("\%s %2d %d\"", mon[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
}

// __TIME__ is expanded to current time 
static char *format_time(struct tm *tm){
   return format("\%02d:%02d:%02d\"", tm->tm_hour, tm->tm_min, tm->tm_sec);
}

void init_macros(void){
   // Define predefined macors
   define_macro("_LP64", "1");
   define_macro("__C99_MACRO_WITH_VA_ARGS", "1");
   define_macro("__ELF__", "1");
   define_macro("__LP64__", "1");
   define_macro("__SIZEOF_DOUBLE__", "8");
   define_macro("__SIZEOF_FLOAT__", "4");
   define_macro("__SIZEOF_INT__", "4");
   define_macro("__SIZEOF_LONG_DOUBLE__", "8");
   define_macro("__SIZEOF_LONG_LONG__", "8");
   define_macro("__SIZEOF_LONG__", "8");
   define_macro("SIZEOF_POINTER__", "8");
   define_macro("SIZEOF_PTRDIFF_T__", "8");
   define_macro("SIZEOF_SHORT__", "2");
   define_macro("SIZEOF_SIZE_T__", "8");
   define_macro("__SIZE_TYPE__", "unsigned long");
   define_macro("__STDC_HOSTED__", "1");
   define_macro("__STDC_NO_COMPLEX__", "1");
   define_macro("__STDC_UTF_16__", "1");
   define_macro("__STDC_UTF_32__", "1");
   define_macro("__STDC_VERSION__", "201112L");
   define_macro("__STDC__", "1");
   define_macro("__USER_LABEL_PREFIX__", "");
   define_macro("__alignof__", "_Alignof");
   define_macro("__amd64", "1");
   define_macro("__amd64__", "1");
   define_macro("__acc__", "1");
   define_macro("__const__", "const");
   define_macro("__gnu_linux__", "1");
   define_macro("__inline__", "inline");
   define_macro("__signed__", "signed");
   define_macro("__typeof__", "typeof");
   define_macro("__unix", "1");
   define_macro("__unix__", "1");
   define_macro("__volatile__", "volatile");
   define_macro("_x86_64", "1");
   define_macro("__x86_64__", "1");
   define_macro("linux", "1");
   define_macro("unix", "1");

   add_builtin("__FILE__",file_macro);
   add_builtin("__LINE__", line_macro);
   add_builtin("__COUNTER__", counter_macro);
   add_builtin("__TIMESTAMP__",timestamp_macro);
   add_builtin("__BASE_FILE__",base_file_macro);

   time_t now = time(NULL);
   struct tm *tm = localtime(&now);
   define_macro("__DATE__", format_date(tm));
   define_macro("__TIME__", format_time(tm));
}

typedef enum{
   STR_NONE, STR_UTF8, STR_UTF16, STR_UTF32, STR_WIDE,
} StrType; 

static StrType getStrType(Token *tok){
    if (!strcmp(tok->loc, "u8")){
       return STR_UTF8;
    }

    switch(tok->loc[0]){
       case '"': return STR_NONE;
       case 'u': return STR_UTF8;
       case 'U': return STR_UTF32;
       case 'L': return STR_WIDE;
    }
    unreachable();
}

// Concatenate adjacent string literals into a single string literal as per the C spec.
static void join_adjacent_string_literals(Token *tok){
   // First pass: If regular string literals are adjacent then join them and string literals, regular string literals are converted to a wide and type before concatenation. In this pass, we do the conversion
   for (Token *tok1 = tok; tok1->kind != TK_EOF){
      if (tok1->kind != TK_STR || tok1->next->kind != TK_STR){
          tok1 = tok1->next;
          continue;
      }

      StrType kind = getStrType(tok1);
      Type *basety = tok1->ty->base;

      for (Token *t = tok1->next; t->kind == TK_STR; t = t->next){
          StrType k = getStrType(t);
          if (kind == STR_NONE){
             kind = k;
             basety = t->ty->base;
          } else if(k != STR_NONE && kind != k) {
             erorr_tok(t, "unsupported non-standard concatenation of string literals");

          }
      }
      if (basety->size > 1){
         for (Token *t = tok1; t->kind == TK_STR; t= t->next){
            if (t->ty->base->size == 1){
               *t = *tokenize_string_literal(t, basety);
            }
         }
      }
      while(tok1->kind == TK_STR){
         tok1 = tok1->next;
      }
   }

   // Second Pass: concatenate adjancent string literals
   for (Token *tok1 = tok; tok1->kind != TK_EOF;){
      if (tok1->kind != TK_STR || tok1->next->kind != TK_STR){
         tok1 = tok1->next;
         continue;
      }

      Token *tok2 = tok1->next;
      while(tok2->kind == TK_STR){
         tok2 = tok2->next;
      }

      int len = tok1->ty->array_len;
      for (Token *t = tok1->next; t != tok2; t = t->next){
          len = len + t->ty->array_len - 1;
      }

      char *buf = calloc(tok1->ty->base->size, len);

      int i = 0;
      for (Token *t = tok1; t != tok2; t = t->next ){
         memcpy(buf + i, t->str, t->ty->size);
         i = i + t->ty->size - t->ty->base->size;
      }

      *tok1 = *copy_token(tok1);
      tok1->ty = array_of(tok1->ty->base, len);
      tok1->str = buf;
      tok1->next = tok2;
      tok1 = tok2;
   }
}
// entry point function of preprocessing analysis
Token *preprocess(Token *tok){
   tok = preprocess2(tok);
   if (cond_incl){
      error_tok(cond_incl->tok, "unterminated conditional directive");

   }
   convert_pp_tokens(tok);
   join_adjacent_string_literals(tok);

   for (Token *t = tok; t; t = t->next){
      t->line_no += t->line_delta;
   }
   return tok;
}






