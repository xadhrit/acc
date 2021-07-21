/* input   |  output     |
-------------------------|
           |
           |

 take a token | return an ast node of it

  static Type *find....(Token *tok){
      if (tok->kind == TK_....){
          VarScope *sc = find_var(tok);
          if (sc){
             return sc->type_def;
          }
      }
      return NULL;
  }

*/
#include "acc.h"


typedef struct {
   Obj *var;
   Type *type_def;
   Type *enum_ty;
   int enum_val;

} VarScope;

// Represents a block scope
typedef struct Scope Scope;
struct Scope {
   Scope *next;

   // C has two block scopes; one is for variables/typedefs and the other is for struct/union/enum tags
   HashMap vars;
   HashMap tags;
};

typedef struct {
   bool is_typedef;
   bool is_static;
   bool is_extern;
   bool is_inline;
   bool is_tls;
   int align;
} VarAttr;

typedef struct Initializer Initializer;
struct Initializer {
   Initializer *next;
   Type *ty;
   Token *tok;
   bool is_flexible;
   Node *expr;
   Initializer **children;
   Member *mem;
};

typedef struct InitDesg InitDesg;
struct InitDesg {
    InitDesg *next;
    int idx;
    Member *member;
    Obj *var;
};

static Obj *locals;
static Obj *globals;
static Scope *scope = &(Scope){};

//goto statements and labels
static Node *gotos;
static Node *labels;

// Current "goto" and "continue" jump targets;
static char *brk_label;
static char *cont_label;

static Obj *current_switch;
static Obj *builtin_alloca;

static bool is_typename(Token *tok);
static Type *declspec(Token **rest, Token *tok, VarAttr *attr);
static Type *typename(Token **rest, Token *tok);
static Type *enum_specifier(Token **rest, Token *tok);
static Type *typeof_specifier(Token **rest, Token *tok);
static Type *type_suffix(Token **rest, Token *tok, Type *ty);
static Type *declarator(Token **rest, Token *tok, Type *ty);
static Node *declatation(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static void array_initializer2(Token **rest, Token *tok, Initializer *init, int i);
static void struct_initializer2(Token **rest, Token *tok, Initializer *init, Member *mem);
static void initializer2(Token **rest, Token *tok, Initializer *init);
static Initializer *initializer(Token **rest, Token *tok, Type *ty, Type **new_ty );
static Node *lvar_initializer(Token **rest, Token *tok,  Obj *var);
static void gvar_initializer(Token **rest, Token *tok, Obj *var);
static Node *compound_stmt(Token **rest, Token *tok );
static Node *stmt(Token **rest, Token *tok);
static Node *expr_stmt(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
static int64_t eval(Node *node);
static int64_t eval2(Node *node, char ***label);
static int64_t eval_rval(Node *node, char ***label);
static bool is_const_expr(Node *node);
static Node *assign(Token **rest, Token *tok);
static Node *logor(Token **rest, Token *tok);
static double eval_double(Node *node);
static Node *conditional(Token **rest, Token *tok);
static Node *logand(Token **rest,Token *tok);
static Node *bitor(Token **rest, Token *tok);
static Node *bitxor(Token **rest, Token *tok);
static Node *bitand(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok);
static Node *shift(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *new_add(Node *lhs, Node *rhs , Token *tok);
static Node *new_sub(Node *lhs , Node *rhs ,Token *tok);
static Node *mul(Token **rest, Token *tok);
static Node *cast(Token **rest,Token *tok);
static Member *get_struct_member(Type *ty,Token *tok);
static Type *struct_decl(Token **rest, Token *tok);
static Type *union_decl(Token **rest, Token *tok);
static Node *postfix(Token **rest, Token *tok);
static Node *funcall(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);
static Token *parse_typedef(Token *tok, Type *basety);
static bool is_function(Token *tok);
static Token *function(Token *tok, Type *basety, VarAttr *attr);
static Token *global_variable(Token *tok, Type *basety, VarAttr *attr);

static int align_down(int n, int align){
  return align_to(n - align + 1, align) ;
}

static void enter_scope(void){
   Scope *sc = calloc(1, sizeof(Scope));
   sc->next = scope;
   scope = sc;
}

static void leave_scope(void){
  scope = scope->next; 
}

static VarScope *find_var(Token *tok){
   for (Scope *sc = scope; sc; sc = sc->next){
      VarScope *sc2 = hashmap_get2(&sc->vars, tok->loc, tok->len);
      if (sc2){
         return sc2;
      }
   }
   return NULL;
}

static Type *find_tag(Token *tok){
    for (Scope *sc = scope; sc; sc= sc->next){
       Type *ty = hashmap_get2(&sc->tags, tok->loc, tok->len);
       if (ty){
          return ty;
       }
    }
    return NULL;
}

static Node *new_node(NodeKind kind , Token *tok){
   Node *node = calloc(1, sizeof(Node));
   node->kind = kind;
   node->tok = tok;
   return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok){
   Node *node = new_node(kind, tok);
   node->lhs = lhs;
   node->rhs = rhs;
   return node;
}

static Node *new_unary(NodeKind kind, Node *expr, Token *tok){
   Node *node = new_node(kind, tok);
   node->lhs = expr;
   return node;
}

static Node *new_num(int64_t val, Token *tok){
   Node *node= new_node(kind, tok);
   node->val = val;
   node->ty = ty_long;
   return node;
}

static Node *new_long(int64_t val, Token *tok){
   Node *node = new_node(ND_NUM, tok);
   node->val = val;
   node->ty = ty_long;
   return node;
}

static Node *new_ulong(long val, Token *tok){
   Node *node = new_node(ND_NUM, tok);
   node->val = val;
   node->ty = ty_ulong;
   return node;
}

static Node *new_var_node(Obj *var, Token *tok){
   Node *node = new_node(ND_VAR, tok);
   node->var = var;
   return node;
}

static Node *new_vla_ptr(Obj *var , Token *tok){
   Node *node = new_node(ND_VAL_PTR, tok);
   node->var = var;
   return node;
}

Node *new_cast(Node *epxr, Type *ty){
   add_type(expr);
   Node *node = calloc(1, sizeof(Node));
   node->kind = ND_CAST;
   node->tok = epxr->tok;
   node->lhs = expr;
   node->ty = copy_type(ty);
   return node;
}

static VarScope *push_scope(char *name){
   VarScope *sc = calloc(1, sizeof(VarScope));
   hashmap_put(&scope->vars,name, sc);
   return sc;
}

static Initializer *new_initializer(Type *ty, bool is_flexible){
   Initializer *init = calloc(1, sizeof(Initializer));
   init->ty = ty;

   if (ty->kind == TY_ARRAY){
      if (is_flexible && ty->size < 0){
        init->is_flexible = true;
        return init;
      }

      init->children = calloc(ty->array_len, sizeof(Initializer *));
      for (int i = 0; i < ty->array_len; i++){
         init->children[i] = new_initializer(ty->base, false);
      }
      return init;
   }

   if (ty->kind == TY_STRUCT || ty->kind == TY_UNION  ){
      int len = 0;
      for (Member *mem = ty->members; mem; mem = mem->next){
         len++;
      }
      init->children  = calloc(len, sizeof(Intializer *));

      for (Member *mem = ty->members; mem; mem = mem->next){
         if (is_flexible && ty->is_flexible && !mem->next){
            Initializer *child = calloc(1, sizeof(Initializer));
            child->ty = mem->ty;
            child->is_flexible = true;
            init->children[mem->idx] = child;
         }
         else {
            init->children[mem->idx] = new_initializer(mem->ty,false);
         }
      }
      return init;
   }
   return init;
}

static Obj *new_var(char *name, Type *ty){
   Obj *var = calloc(1, sizeof(Obj));
   var->name = name;
   var->ty = ty;
   var->align = ty->align;
   push_scope(name)->var = var;
   return var;
}

static Obj *new_lvar(char *name, Type *ty){
   Obj *var = new_var(name, ty);
   var->is_local = true;
   var->next = locals;
   locals = var;
   return var;
}

static Obj *new_gvar(char *name, Type *ty){
   Obj *var = new_var(name, ty);
   var->next = globals;
   var->is_static = true;
   var->is_definition = true;
   globals = var;
   return var;
}

static char *new_unique_name(void){
   static int id = 0;
   return format(".L..%d", id++);
}

static Obj *new_anon_gvar(Type *ty){
   return new_gvar(new_unique_name(), ty);
}

static Obj *new_string_literal(char *p, Type *ty){
   Obj *var = new_anon_gvar(ty);
   var->init_data =p;
   return var;
}

static char *get_ident(Token *tok){
   if (tok->kind != TK_IDENT){
      error_tok(tok, "expected an identifier");
   }
   return strndup(tok->loc, tok->len);
}

static Type *find_typedef(Token *tok){
   if (tok->kind == TK_IDENT){
      VarScope *sc = find_var(tok);
      if (sc){
         return sc->type_def;
      }
   }
   return NULL;
}

static void push_tag_scope(Token *tok, Type *ty){
   hashmap_put2(&scope->tags, tok->loc, tok->len);
}

static Type *declspec(Token **rest, Token *tok, VarAttr *attr){
   enum {
     VOID = 1 << 0,
     BOOL = 1 << 2, 
     CHAR = 1 << 4, 
     SHORT= 1 << 6 , 
     INT = 1 << 8, 
     LONG = 1 << 10 , 
     FLOAT =  1 << 12,
     DOUBLE = 1 << 14 ,
     OTHER = 1 << 16 ,
     SIGNED = 1 << 17 ,
     UNSIGNED = 1 << 18,
   };

   Type *ty = ty_int;
   int counter = 0;
   bool is_atomic = false;

   while (is_typename(tok)){
      if (equal(tok, "typedef") || equal(tok, "static") || equal(tok, "extern") ||equal(tok, "inline") || equal(tok, "_Thread_local") || equal(tok, "_thread")){
          if (!attr){
             error_tok(tok, "storage class specifier is not allowed in this context");
          }
          if (equal(tok, "typedef")){
             attr->is_typedef = true;
          }
          else if (equal(tok, "static")){
             attr->is_static  = true;
          }
          else if (equal(tok, "extern")){
             attr->is_extern = true;
          }
          else if (equal(tok, "inline")){
             attr->is_inline = true;
          }
          else {
            attr->is_tls = true;
          }
          
          if (attr->is_typedef && attr->is_static + attr->is_extern + attr->is_inline + attr->is_tls > 1  ){
             error_tok(tok,"typedef may be not use together with static," 
                " extern, inline, _thread or _Thread_local");
            
          }
          tok =tok->next;
          continue;
      }

      if (consume(&tok, tok, "const") || consume(&tok, tok, "volatile") || consume(&tok, tok, "auto") || consume(&tok, tok, "register") || consume(&tok, tok, "restrict") || consume(&tok, tok, "__restrict") || consume(&tok, tok, "__restrict__") || consume(&tok, tok, "_Noreturn") ){
      continue
      }
    
    if (equal(tok, "_Atomic")){
       tok = tok->next;
       if (equal(tok, "(")){
          ty = typename(&tok, tok->next);
          tok = skip(tok, ")");
       }

       is_atomic = true;
       continue;
    }

    if (equal(tok, "_Alignas")){
       if (!attr){
          error_tok(tok, "_Aliganas is not allowed in this context");
       }
       tok = skip(tok->next, "(");

       if (is_typename(tok)){
          attr->align = typename(&tok, tok)->align;
       }
       else {
          attr->align = const_expr(&tok, tok);
       }
       tok = skip(tok, ")");
       continue;
    }

    Type *ty2 = find_typedef(tok);
    if (equal(tok, "struct") || equal(tok, "union") || equal(tok,"enum") || equal(tok, "typeof") || ty2){
       if (counter) {
        break;
       }

       if (equal(tok, "struct")){
          ty = struct_decl(&tok, tok->next);
       }
       else if (equal(tok, "union")){
          ty = union_decl(&tok, tok->next);
       }
       else if (equal(tok, "enum")){
          ty = enum_specifier(&tok, tok->next);
       }
       else if (equal(tok, "typeof")){
          ty = typeof_specifier(&tok, tok->next);
       }
       else{
          ty = ty2;
          tok = tok->next;
       }

       counter += OTHER;
       continue;
    }

    // Handle built-in types;
    if (equal(tok, "void")){
       counter += VOID;
    }
    else if (equal(tok, "_Bool")){
       counter += BOOL;
    }
    else if (equal(tok, "char")){
       counter += CHAR;
    }
    else if (equal(tok, "short")){
      counter += SHORT;
    }
    else if (equal(tok, "int")){
       counter += INT;
    }
    else if (equal(tok, "long")){
      counter += LONG;
    }
    else if (equal(tok, "float")){
       counter += FLOAT;
    }
    else if (equal(tok, "double")){
       counter += DOUBLE;
    }
    else if (equal(tok, "signed")){
       counter |= SIGNED;
    }
    else if (equal(tok, "unsigned")){
       counter |= UNSIGNED;
    }
    else {
       unreachable();
    }

    switch (counter){
        case VOID:
           ty =ty_void;
           break;
        case BOOL:
           ty = ty_bool;
           break;
        case CHAR:
        case SIGNED + CHAR:
           ty = ty_char;
           break;
        case UNSIGNED + CHAR:
           ty = ty_uchar;
           break;
        case SHORT:
        case SHORT + INT:
        case SIGNED + SHORT:
        case SIGNED + SHORT + INT:
             ty = ty_ushort;
             break;
        case UNSIGNED + SHORT:
        case UNSIGNED + SHORT + INT:
             ty = ty_ushort;
             break;
        case INT:
        case SIGNED:
        case SIGNED + INT:
             ty =ty_int;
             break;
        case UNSIGNED:
        case UNSIGNED +  INT:
             ty = ty_uint;
             break;
        case LONG:
        case LONG + INT:
        case LONG + LONG:
        case LONG + LONG + INT:
        case SIGNED + LONG + INT:
        case SIGNED + LONG + LONG:
        case SIGNED + LONG + LONG + INT :
             ty = ty_long;
             break;
        case UNSIGNED + LONG:
        case UNSIGNED + LONG + INT:
        case UNSIGNED + LONG + LONG:
        case UNSIGNED + LONG + LONG + INT:
             ty = ty_ulong;
             break;
        case FLOAT:
             ty = ty_float;
             break;
        case DOUBLE:
             ty =ty_double;
             break;
        case LONG + DOUBLE:
             ty = ty_ldouble;
             break;
        case DOUBLE:
             ty = ty_ldouble;
             break;
        default:
             error_tok(tok, "invalid type");
    }

    tok = tok->next;

   }

   if (is_atomic){
      ty = copy_type(ty);
      ty->is_atomic = true;
   }

   *rest = tok;
   return ty;
}

static Type *func_params(Token **rest, Token *tok, Type *ty){
   if (equal(tok, "void") && equal(tok->next, ")")){
      *rest = tok->next->next;
      return func_type(ty);
   }

   Type head = {};
   Type *cur = &head;
   bool is_variadic = false;

   while (!equal(tok, ")")){
         if (cur != &head){
            tok = skip(tok, ",");
         }
         if (equal(tok, "...")){
          is_variadic = true;
          tok = tok->next;
          skip(tok, ")");
          break;
         }

         Type *ty2 = declspec(&tok, tok, NULL);
         ty2 = declspec(&tok, tok, ty2);

         Token  *name = ty2->name;

         if (ty2->kind == TY_ARRAY){
              ty2 = pointer_to(ty2->base);
              ty2->name = name;
         }
         else if (ty2->kind == TY_FUNC){
              ty2 = pointer_to(ty2);
              ty2->name = name;
         }
              cur = cur->next = copy_type(ty2);

    }

    if (cur == &head){
       is_variadic = true;
    }
    ty = func_type(ty);
    ty->params = head.next;
    ty->is_variadic = is_variadic;
    *rest = tok->next;
    return ty;
}   

// array-dimension
static Type *array_dimension(Token **rest, Token *tok, Type *ty){
   while (equal(tok, "static") || equal(tok, "restrict")){
      tok =tok->next;
   }

   if (equal(tok, "]")){
       ty = type_suffix(rest, tok->next, ty);
       return array_of(ty, -1);
   }

   Node *expr = conditional(&tok, tok);
   tok = skip(tok, "]");
   ty = type_suffix(rest, tok, ty);

   if (ty->kind == TY_VLA ||!is_const_expr(expr)){
      return vla_of(ty, expr);
   }
   return array_of(ty, eval(expr));
}


// type-suffix = "(" func-params
//               | "[" array-dimensions
//               |       

static Type *type_suffix(Token **rest, Token *tok, Type *ty){
   if (equal(tok, "(")){
      return func_params(rest, tok->next, ty);
   }

   if (equal(tok, "[")){
      return array_dimensions(rest, tok->next, ty);
   }

   *rest = tok;
   return ty;
}

//pointers = ("*"  ("const" | "volatile" | "restrict" )
static Type *pointers(Token **rest, Token *tok, Type *ty){
   while (consume(&tok,tok, "*")){
       ty = pointer_to(ty);
       while (equal(tok, "const") || equal(tok, "volatile") || equal(tok, "__restrict") || equal(tok,"__restrict__")){
           tok = tok->next;
       }
   }
   *rest = tok;
   return ty;
}

// declarator = pointer ("(" ident ")" | "("declarator " )" )
static Type *declarator(Token **rest, Token *tok, Type *ty){
   ty = pointers(&tok, tok, ty);
   if (equal(tok, "(")){
      Token *start = tok;
      Type dummy ={};
      declarator(&tok, start->next, &dummy);
      tok =skip(tok, ")");
      ty = type_suffix(rest, tok, ty);
      return declarator(&tok, start->next, ty);
   }

   Token *name = NULL;
   Token *name_pos = tok;

   if (tok->kind == TK_IDENT){
      name = tok;
      tok = tok->next;
   }

   ty = type_suffix(rest, tok, ty;
   ty->name= name;
   ty->name_pos = name_pos;
   return ty;
}

// abstract-declarator = pointers ("( abstract-declarator
static Type *abstract_declarator(Token **rest, Token *tok, Type *ty){
   ty = pointers(&tok, tok, ty);

   if (equal(tok, "(")){
      Token *start = tok;
      Type dummy = {};

      abstract_declarator(&tok, start->next, &dummy);
      tok = skip(tok ,")");
      ty = type_suffix(rest, tok, ty);
      return abstract_declarator(&tok, start->next, ty);
   }

   return type_suffix(rest, tok, ty);
}

// type->name = declspec abstract-declarator
static Type *typename(Token **rest, Token *tok){
    Type *ty = declspec(&tok, tok, NULL);
    return abstract_declarator(rest, tok, ty);
}

static bool is_end(Token **tok){
   return equal(tok, "}") || (equal(tok, ",") && equal(tok->next, "}"));
}

static bool consume_end(Token **rest, Token *tok){
   if(equal(tokm "}")){
      *rest = tok->next;
      return true;
   }

   if (equal(tok, ",") && equal(tok->next, "}")){
       *rest = tok->next->next;
       return true;
   }

   return false;
}

// enum-specifier
static Type *enum_specifier(Token **rest, Token *tok){
   Type *ty = enum_type();

   // going through of struct tag.
   Token *tag  = NULL;
   if (tok->kind == TK_IDENT){
      tag = tok;
      tok = tok->next;
   }

   if (tag && !equal(tok, "{")){
       Type *ty = find_tag(tag);
       if (!ty){
          error_tok(tag,"this enum type is not in list" );
       }
       if (ty->kind != TY_ENUM){
          error_tok(tag, "this is not a enum type");
       }
       *rest =tok;
       return ty;
   }

   tok = skip(tok, "{");

   // read an enum-list;
   int i=0;
   int val = 0;
   while (!consume_end(rest, tok)){
     if (i++ > 0){
        tok = skip(tok, ",");
     }

     char *name = get_ident(tok);
     tok = tok->next;

     if (equal(tok, "=")){
        val = const_expr(&tok ,tok->next);
     }

     VarScope *sc = push_scop(name);
     sc->enum_ty = ty;
     sc->enum_val = val++;
   }

   if (tag){
      push_tag_scope(tag, ty);
   }
   return ty;
}

// typedefined of specifier = "(" (expr | typename) ")"

static Type *typeof_specifier(Token **rest, Token *tok){
   tok = skip(tok, "(");

   Type *ty;
   if (is_typename(tok)){
      ty=  typename(&tok,tok);
   }
   else {
      Node *node = expr(&tok, tok);
      add_type(node);
      ty = node->ty;
   }
   *rest = skip(tok, ")");
   return ty;
}

// generate code for computing a variable length size
static Node *compute_vla_size(Type *ty, Token *tok){
   Node *node = new_node(ND_NULL_EXPR, tok);
   if (ty->base){
      node = new_binary(ND_COMMA, node, compute_vla_size(ty->base));
   }

   if (ty->kind != TY_VLA){
      return node;
   }

   Node *base_sz;
   if (ty->base->kind == TY_VLA){
      base_sz = new_var_node(ty->base->vla_size, tok);
   }
   else {
      base_sz = new_num(ty->base->size, tok);
   }

   ty->vla_size = new_lvar("", ty_ulong);
   Node *expr = new_binary(ND_ASSIGN, new_var_node(ty->vla_size,tok), new_binary(ND_MUL, ty->vla_len, base_sz, tok) , tok);
   return new_binary(ND_COMMA, node, expr, tok);
   
}

static Node *new_alloca(Node *sz){
    Node *node = new_unary(ND_FUNCALL, new_var_node(builtin_alloca, sz->tok), sz->tok);
    node->func_ty = builtin_alloca->ty;
    node->ty = builtin_alloca->ty->return_ty;
    node->arg = sz;
    add_type(sz);
    return node;
}
