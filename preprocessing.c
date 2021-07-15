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





