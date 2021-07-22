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

// declaration = declspec (declarator ("=" expr) ? (","declarator ("=" expr )?)*)? ";"
static Node *declaration(Token **rest, Token **tok, Type *basety, VarAttr *attr){
   Node head = {};
   Node *cur = &head;
   int i = 0;

   while (!equal(tok, ";")){
      if (i++ > 0){
         tok = skip(tok, ",");
      }
      Type *ty = declarator(&tok, tok, basety);
      if (ty->kind == TY_VOID){
         error_tok(tok, "variable declared void");
      }
      if (!ty->name){
          error_tok(ty->name_pos, "variable name omitted");
      }

      if (attr && attr->is_static){
         // static local variable
         Obj *var = new_anon_gvar(ty);
         push_scope(get_ident(ty->name))->var = var;
         if (equal(tok, "=")){
            gvar_initializer(&tok, tok->next, var);
         }
         continue;
      }

      // variable length array size ?
      cur = cur->next =new_unary(ND_EXPR_STMT, compute_vla_size(ty, tok), tok);

    
   if (ty->kind == TY_VLA){
      if (equal(tok, "=")){
         error_tok(tok, "variable-sized object may not be initialized");

      }
      Obj *var = new_lvar(get_ident(ty->name), ty);
      Token *tok = ty->name;
      Node *expr = new_binary(ND_ASSIGN, new_vla_ptr(var, tok), new_alloca(new_var_node(ty->vla_size, tok)), tok);

      cur = cur->next = new_unary(ND_EXPR_STMT, expr, tok);
      continue;
    }

    Obj *var=  new_lvar(get_ident(ty->name), ty);
    if (attr && attr->align){
       var->align = attr->align;
    }

    if (equal(tok, "=")){
       Node *expr = lvar_initializer(&tok, tok->next, var);
       cur = cur->next= new_unary(ND_EXPR_STMT, epxr, tok);
    }

    if (var->ty->size < 0){
       error_tok(ty->name, "variable has incomplete type");
    }
    if (var->ty->kind == TY_VOID){
       error_tok(ty->name, "variable declared void");
    }

  }
    Node *node = new_node(ND_BLOCK, tok);
    node->body = head.next;
    *rest = tok->next;
    return node;
}

static Token *skip_excess_element(Token *tok){
   if (equal(tok, "{")){
      tok = skip_excess_element(tok->next);
      return skip(tok, "}");
   }

   assign(&tok, tok);
   return tok;
}

// string-initializer = string-literal
static void string_initializer(Token **rest, Token *tok, Initializer *init){
   if (init->is_flexible){
       *init = *new_initializer(array_of(init->ty->base, tok->ty->array_len), false);
   }
   int len = MIN(init->ty->array_len, tok->ty->array_len);

   switch(init->ty->base->size){
       case 1: {
          char *str = tok->str;
          for (int i = 0; i < len; i++){
              init->children[i]->expr = new_num(str[i], tok);
          }
          break;
       }
       case 2: {
          uint16_t *str= (uint16_t *)tok->str;
          for (int i=0; i < len; i++){
              init->children[i]->expr=  new_num(str[i], tok);
          }
          break;
       }

       case 4: {
          uint32_t *str = (uint32_t *)tok->str;
          for (int i=0; i < len; i++){
             init->children[i]->expr = new_num(str[i], tok);
          }
          break;
       }
       
       default:
          unreachable();
   }

   *rest = tok->next;
}

static void array_designator(Token **rest, Token *tok, Type *ty, int *begin, int *end){
   *begin = const_expr(&tok, tok->next);
   if (*begin >= ty->array_len){
       error_tok(tok, "array designator index exceeds array bounds");
   }
   if (equal(tok, "...")){
     *end = const_expr(&tok, tok->next);
     if (*end >= ty->array_len){
         error_tok(tok, "array designator index exceeds array bounds");
     }
     if (*end < *begin){
        error_tok(tok, "array designator range [%d, %d] is empty");
     }
   }
   else {
      *end = *begin;
   }

   *rest = skip(tok, "]");
}

//struct-desginator = "."ident
static Member *struct_desginator(Token **rest, Token *tok, Type *ty ){
    Token *start = tok;
    tok = skip(tok, ".");
    if (tok->kind != TK_IDENT){
       error_tok(tok, "expected a field designator");
    }
    for (Member *mem = ty->members; mem; mem = mem->next){
      
        if (mem->ty->kind == TY_STRUCT && !mem->name){
           if (get_struct_member(mem->ty, tok)){
              *rest = start;
              return mem;
           }
           continue;
        }

        // regular struct member
        if (mem->name->len == tok->len && !strncmp(mem->name->loc, tok->loc, tok->len)){
           *rest = tok->next;
           return mem;
        }
    }

    error_tok(tok, "struct has no such member");
}

// check the designation of ("[" const-expr  "]" | "." ident) "="? initializer

static void desgination(Token **rest, Token *tok, Initializer *init){
   if (equal(tok, "[")){
      if (init->ty->kind != TY_ARRAY){
         error_tok(tok, "array index in non-array initializer");
      }

      int begin, end;
      array_designator(&tok, tok , init->ty, &begin, &end);

      Token *tok2;
      for (int i = begin; i <= end; i++){
         designation(&tok2, tok, init->children[i]);

      }
      array_initializer2(rest, tok2, init, begin + 1);
      return;
   }

   if (equal(tok, ".") && init->ty->kind == TY_STRCUT){
      Member *mem = strcut_designator(&tok, tok, init->ty);
      designation(&tok, tok, init->children[mem->idx]);
      init->expr = NULL;
      struct_initializer2(rest, tok, init, mem->next);
      return;
   }

   if (equal(tok, ".") && init->ty->kind == TY_UNION){
      Member *mem = struct_desginator(&tok, tok, init-ty);
      init->mem = mem;
      designation(rest, tok, init->children[mem->idx]);
      return;
   }

   if (equal(tok, ".")){
      error_tok(tok, "field name not in struct or union initializer")
   }

   if (equal(tok, "=")){
      tok = tok->next;
   }
   initlaizer2(rest, tok, init);
}

//An array length can be omitted if an array has an initializer.
static int count_array_init_elements(Token *tok, Type *ty){
   bool first = true;
   Initializer *dummy = new_initializer(ty->base, true);

   int i= 0; 
   int m = 0;

   while(!consume_end(&tok,tok)) {
      if (!first){
         tok = skip(tok, ",");
      }
      first = false;

      if (equal(tok, "[")){
          i = const_expr(&tok, tok->next);
          if (equal(tok, "...")){
              i =const_expr(&tok, tok->next);
          }
          tok = skip(tok, "]");
          designation(&tok, tok, dummy);
      }
      else {
          initializer2(&tok, tok, dummy);
      }

      i++;
      m = MAX(m,i);
   }
   return m;
}


// array-initializer1 = "{" initializer ("," initializer)* ","? "}")
static void array_initializer(Token **rest,Token *tok, Initializer *init ){
   tok = skip(tok, "{");

   if(init->is_flexible){
       int len = count_array_init_elements(tok, init->ty);
       *init = *new_initializer(array_of(init->ty->base, len), false);
   }
   bool first = true;

   if (init->is_flexible){
       int len = count_array_init_elements(tok, init->ty);
       *init = *new_initializer(array_of(init->ty->base, len), false);
   }

   for (int i=0;!consume_end(rest, tok);i++){
       if (!first){
           tok = skip(tok, ",");
       }
       first = false;

       if (equal(tok, "[")){
          int begin, end;
          array_designator(&tok, tok, init->ty, &begin, &end);

          Token *tok2;
          for (int j = begin; j <=end; j++){
              desgination(&tok2, tok, init->children[j]);
          }
          tok = tok2;
          i = end;
          continue;
       }

       if (i < init->ty->array_len){
          initializer(&tok, tok, init->children[i]);
       }
       else {
          tok = skip_excess_element(tok);
       }
   }
}

// initializer2
static void array_initializer2(Token *rest, Token *tok, Initializer *inint){
    if (init->is_flexible){
       int len = count_array_init_elements(tok, init->ty);
       *init = *new_initializer(array_of(init->ty->base, len), false);
    }

    for (; i < init->ty->array_len && !is_end(tok); i++){
       Token *start = tok;
       if (i > 0){
          tok = skip(tok, ",");
       }
       if (equal(tok, "[") || equal(tok, ".")){
           *rest = start;
           return;
       }
       initializer2(&tok, tok, init->children[i]);
    }
    *rest = tok;
}

//struct-initializer1 = "{" initializer ( ", " initializer )* ","
static void struct_initializer1(Token **rest, Token *tok, Initializer *init, Member *mem){
    tok = skip(tok, "{");

    Member *mem = init->ty->members;
    bool first = true;

    while (!consume_end(rest, tok)){
       if (!first){
          tok = skip(tok, ",");
       }
       first = false;

       if (equal(tok, ".")){
           mem = struct_designator(&tok, tok, init->ty);
           designation(&tok, tok, init->children[mem->idx]);
           mem = mem->next;
           continue;
       }

       if (mem){
           initializer2(&tok, tok, init->children[mem->idx]);
           mem = mem->next;
       }
       else {
           tok = skip_excess_element(tok);
       }
    }
}

// struct-initializer2= initializer ("," initializer)*
static void struct_initializer2(Token **rest, Token *tok, Initializer *init,Member *mem){
    bool first = true;
    for (; mem && !is_end(tok); mem = mem->next){
       Token *start = tok;

       if (!first){
          tok = skip(tok, ",");
       }
       first =false;

       if (equal(tok, "[") || equal(tok, ".")){
          *rest = start;
          return;
       }

       initializer2(&tok, tok, init->children[mem->idx])
    }
    *rest = tok;  
}

static void union_initializer(Token **rest, Token *tok, Initializer *init){
   if (equal(tok, "{")  && equal(tok->next, ".")){
       Member *mem = struct_designator(&tok, tok->next, init->ty);
       init->mem= mem;
       designation(&tok, tok, init->children[mem->idx]);
       *rest = skip(tok, "}");
       return;
   }

   init->mem = init->ty->members;

   if (equal(tok, "{")){
      initializer2(&tok, tok, init->children[0]);
      consume(&tok, tok,",");
      *rest = skip(tok, "}");
   }
   else {
      initializer2(rest, tok, init->children[0]);
   }
}

// initializer = string-initializer | array-initializer | struct-initializer | union-initializer | assign
static void initializer2(Token **rest, Token *tok, Initializer *init){
    if (init->ty->kind == TY_ARRAY && tok->kind == TK_STR){
        string_initializer(rest, tok,init);
        return;
    }

    if (init->ty->kind == TY_ARRAY){
       if (equal(tok, "{")){
          array_initializer(rest, tok, init);
       }
       else {
          array_initializer(rest, tok, init , 0);
       }
       return;
    }

    if (init->ty->kind == TY_STRUCT){
         if (equal(tok, "{")){
            struct_initializer(rest, tok, init);
            return;
         }

         Node *expr = assign(rest, tok);
         add_type(expr);
         if (expr->ty->kind == TY_STRUCT){
             init->expr = expr;
             return;
         }

         struct_initializer(rest, tok, init, init->ty->members);
         return;
    }
    if (init->ty->kind == TY_UNION){
       union_initializer(rest, tok, init);
       return;
    }

    if (equal(tok, "{")){
       initializer2(&tok, tok->next, init);
       *rest = skip(tok, "}");
       return;
    }

    init->expr = assign(rest, tok);
}

static Type *copy_struct_type(Type *ty){
   ty = copy_type(ty);
   Member head = {};

   Member *cur = &head;
   for (Member *mem = ty->members; mem; mem=mem->next){
      Member *m = calloc(1, sizeof(Member));
      *m = *mem;
      cur = cur->next = m;
   }

   ty->members = head.next;
   return ty;
}

static Initializer *initializer(Token **rest, Token *tok, Type *ty, Type **new_ty){
   Initializer *init = new_initializer(ty, true);
   initializer2(rest, tok, init);

   if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->is_flexible ){
      ty = copy_struct_type(ty);

      Member *mem = ty->members;
      while(mem->next){
         mem = mem->next;
      }
      mem->ty = init->childrem[mem->idx]->ty;
      ty->size += mem->ty->size;

      *new_ty = ty;
      return init;
   }
   *new_ty = init->ty;
   return init;
}

static Node *init_desg_expr(InitDesg *desg, Token *tok){
   if (desg->var){
       return new_var_node(desg->var, tok);
   }
   if (desg->member){
      Node *node = new_unary(ND_MEMBER, init_desg_expr(desg->next, tok), tok);
      node->member = desg->member;
      return node;
   }

   Node *lhs = init_desg_expr(desg->next, tok);
   Node *rhs = new_num(desg->idx, tok);
   return new_unary(ND_DEREF, new_add(lhs, rhs, tok), tok);
}

static Node *create_lvar_init(Initializer *init, Type *ty, InitDesg  *desg , Token *tok){
   if (ty->kind == TY_ARRAY){
      Node *node = new_node(ND_NULL_EXPR, tok);
      for (int i = 0; i < ty->array_len; i++){
          InitDesg desg2 = {desg, 1};
          Node *rhs = create_lvar_init(init->children[i], ty->base, &desg2,tok);
          node = new_binary(ND_COMMA, node, rhs, tok);
      }
      return node;
   }

   if (ty->kind == TY_STRUCT && !init->expr){
       Node *node = new_node(ND_NULL_EXPR, tok);
       for (Member *mem = ty->members; mem; mem =mem->next){
           InitDesg desg2 = {desg, 0, mem};
           node=  new_binary(ND_COMMA, node, rhs, tok);
       }
       return node;
   }

   if (ty->kind == TY_UNION){
      Member *mem = init->mem ? init->mem : ty->members;
      InitDesg desg2 = {desg, 0, mem};
      return create_lvar_init(init->children[mem->idx], mem->ty, &desg2, tok);
   }

   if (!init->expr){
      return new_node(ND_NULL_EXPR, tok);
   }

   Node *lhs = init_desg_expr(desg, tok);
   return new_binary(ND_ASSIGN, lhs, init->expr, tok);
}

static Node *lvar_initializer(Token **rest, Token *tok, Obj *var){
   Initializer *init = initializer(rest, tok, var->ty, &var->ty);
   InitDesg desg = {NULL, 0, NULL, var};

   Node *lhs = new_node(ND_MEMZERO, tok);
   lhs->var = var;
   Node *rhs = create_lvar_init(init, var->ty, &desg, tok);
   return new_binary(ND_COMMA, lhs, rhs, tok);
}

static uint64_t read_buf(char *buf, int sz){
   if (sz == 1){
      return *buf;
   }
   if (sz == 2){
      return *(uint16_t *)buf;
   }
   if (sz == 4){
      return *(uint32_t *)buf;
   }
   if (sz == 8){
      return *(uint64_t *)buf;
   }
   unreachable();
}

static void write_buf(char *buf, uint64_t val, int sz){
   if (sz ==1){
      *buf = val;
   }
   else if (sz == 2){
       *(uint16_t *)buf = val;
   }
   else if (sz == 4){
      *(uint32_t *)buf = val;
   }
   else if (sz == 8){
      *(uint64_t *)buf = val;
   }
   else {
      unreachable();
   }
}

static Relocation *write_gvar_data(Relocation *cur, Initializer *init, Type *ty, char *buf, int offset){
     if (ty->kind == TY_ARRAY){
        int sz = ty->base->size;
        for (int i=0; i < ty->array_len; i++){
            cur = write_gvar_data(cur, init->children[i], ty->base, buf, offset + sz * i);

        }
        return cur;
     }

     if (ty->kind == TY_STRUCT){
        for (Member *mem = ty->members; mem; mem = mem->next){
           if (mem->is_bitfield){
              Node *expr = init->children[mem->idx]->expr;
              if (!epxr){
                 break;
              }
              char *loc = buf + offset + mem->offset;
              uint64_t oldval = read_buf(loc, mem->ty->size );
              uint64_t newval = eval(expr);
              uint64_t mask = (1L << mem->bit_width) - 1;
              uint64_t combined = oldval | ((newval & mask) << mem->bit_offset);
              write_buf(loc, combined, mem->ty->size);
           }
           else {
              cur = write_gvar_data(cur, init->children[mem->idx], mem->ty, buf, offset + mem->offset);
           }
        }
        return cur;
     }

     if (ty->kind == TY_UNION){
        if (!init->mem){
           return cur;
        }
        return write_gvar_data(cur, init->children[init->mem->idx], init->mem->ty, buf, offset);
     }

     if (!init->expr){
        return cur;
     }
     if (ty->kind == TY_FLOAT){
        *(float *)(buf + offset) = eval_double(init->expr);
        return cur;
     }

     if (ty->kind == TY_DOUBLE){
        *(double *)(buf + offset) = eval_double(init->expr);
        return cur;
     }

     char **label = NULL;
     uint64_t val = eval2(init->expr, &label);

     if (!label){
        write_buf(buf + offset, val, ty->size);
        return cur;
     }

     Relocation *rel = calloc(1, sizeof(Relocation));
     rel->offset = offset;
     rel->label = label;
     rel->addend = val;
     cur->next = rel;
     return cur->next;
}

static void gvar_initializer(Token **rest, Token *tok, Obj *var, ){
   Initializer *init = initializer(rest, tok, var->ty, &var->ty);
   Relocation head ={};
   char *buf = calloc(1, var->ty>size);
   write_gvar_data(&head, init, var->ty, buf, 0);
   var->init_data = buf;
   var->rel = head.next;
}

// returns true if a given token represents a type
static bool is_typename(Token *tok){
    static HashMap map;

    if (map.capacity == 0){
       static char *kw[] = {
         "void", "_Bool", "char", "short", "int", "long", "struct","typedef", "enum", "static", "extern", "_Alignas", "signed", "const", "volatile", "auto","register", "float", "restrict", "__restrict", "__restrict__", "_Noreturn", "float", "double", "typeof", "_Thread_local", "__thread", "_Atomic",
       };
       for (int i =0 ; i < sizeof(kw)/ sizeof(*kw); i++){
           hashmap_put(&map, kw[i], (void *)1);
       }
    }
    return hashmap_get2(&map, tok->loc, tok->len) ||find_typedef(tok);
}

static Node *asm_stmt(Token **rest, Token *tok){
   Node *node = new_node(ND_ASM, tok);
   tok = tok->next;

   while (equal(tok, "volatile") || equal(tok, "inline")){
      tok = tok->next;
   }

   tok = skip(tok, "(");
   if (tok->kind != TK_STR || tok->ty->base->kind != TY_CHAR){
      error_tok(tok, "expected string literal");
   }
   node=>asm_str = tok->str;
   *rest = skip(tok->next, ")");
   return node;
}

/*
  stmt = "return" expr? ";" 
       |  "if"  "(" expr ")"  stmt("else" stmt)?
       |   "switch" "(" expr ")" stmt
       |   "case"  const-expr ("..." const->expr) ? ":" stmt
       |    "default" ":" stmt
       |    "for" "(" expr-stmt expr? ";" expr? ")" stmt
       |    "while" "(" expr ")" stmt
       |    "do"   stmt "while" "(" expr ")" ";"
       |    "asm"  asm-stmt
       |    "goto" (ident | "*"  expr ) ";"
       |     "break" ";"
       |     "continue" ";"
       |     "ident"   ":"  stmt
       |     "{" compound-stmt
       |     expr-stmt
*/

static Node *stmt(Token **rest, Token *tok){
    if (equal(tok, "return")){
       Node *node = new_node(ND_RETURN, tok);
       if (consume(rest, tok->next, ";")){
          return node;
       }

       Node *exp = expr(&tok, tok->next);
       *rest = skip(tok, ";");
       add_type(exp);
       Type *ty = current_fn->ty->return_ty;
       if (ty_kind != TY_STRUCT && ty->kind != TY_UNION){
          exp = new_cast(exp, current_fn->ty->return_ty);
       }

       node->lhs = exp;
       return node;
    }

    if (equal(tok, "if")){
       Node *node = new_node(ND_IF, "(");
       tok = skip(tok, "(");
       node->cond = expr(&tok, tok);
       tok = skip(tok, ")");
       node->then = stmt(&tok, tok);
       if (equal(tok, "else")){
          node->els = stmt(&tok,tok->next);
       }
       *rest = tok;
       return node;
    }

    if (equal(tok, "switch")){
        Node *node = new_node(ND_SWITCH, tok);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");

        Node *sw = current_switch;
        current_switch = node;

        char *brk = brk_label;
        brk_label = node->brk_label = new_unique_name();

        node->then = stmt(rest, tok);
        current_switch =  sw;
        brk_label = brk;
        return node;
    }

    if (equal(tok, "case")){
       if (!current_switch){
          error_tok(tok, "stray case");
       }

       Node *node = new_node(ND_CASE, tok);
       int begin = const_expr(&tok, tok->next);
       int end;

       if (equal(tok, "...")){
          end = const_expr(&tok, tok->next);
          if (end < begin){
             error_tok(tok, "empty case range specified");
          } else {
             end = begin;
          }

          tok = skip(tok, ":");
          node->label =new_unique_name();
          node->lhs = stmt(rest, tok);
          node->begin = begin;
          node->end = end;
          node->case_next = current_switch->case_next;
          current_switch->case_next = node;
          return node;
       }
    }

    if (equal(tok, "default")){
       if (!current_switch){
          error_tok(tok, "stray default");
       }

       Node *node = new_node(ND_CASE, tok);
       tok = skip(tok->next, ":");
       node->label = new_unique_name();
       node->lhs= stmt(rest, tok);
       current_switch->default_case = node;
       return node;
    }

    if (equal(tok, "for")){
       Node *node = new_node(ND_FOR, tok);
       tok = skip(tok->next, "(");
       
       enter_scope();

       char *brk = brk_label;
       char *cont = cont_label;
       brk_label = node->brk_label = new_unique_name();
       cont_label = node->cont_label = new_unique_name();

       if (is_typename(tok)){
          Type *basety = declspec(&tok, tok, NULL);
          node->init  = declaration(&tok, tok, basety, NULL);
       }
       else {
          node->init = expr_stmt(&tok, tok);
       }

       if (!equal(tok,";")){
           node->cond = expr(&tok, tok);
       }
       tok = skip(tok, ";");
       
       if (!equal(tok, ")")){
          node->inc = expr(&tok, tok);
       }
       tok = skip(tok, ")");

       node->then = stmt(rest, tok);

       leave_scope();
       brk_label = brk;
       cont_label = cont;
       return node;
   }

   if (equal(tok, "while")){
      Node *node = new_node(ND_FOR, tok);
      tok = skip(tok->next, "(");
      node->cond = expr(&tok, tok);
      tok = skip(tok, ")");

      char *brk = brk_label;
      char *cont= cont_label;
      brk_label = node->brk_label = new_unique_name();
      cont_label= node->cont_label = new_unique_name();

      node->then = stmt(rest, tok);

      brk_label = brk;
      cont_label = cont;
      return node;
   }

   if (equal(tok, "do")){
       Node *node = new_node(ND_DO, tok);
       char *brk = brk_label;
       char *cont = cont_label;
       brk_label = node->brk_label = new_unique_name();
       cont_label = node->cont_label = new_unique_name();

       node->then = stmt(&tok, tok->next);
       brk_label = brk;
       cont_label = cont;

       tok = skip(tok, "while");
       tok = skip(tok, "(");
       node->cond =expr(&tok, tok);
       tok = skip(tok, ")");
       *rest = skip(tok, ";");
       return node;

   }

   if (equal(tok, "asm")){
      return asm_stmt(rest, tok);
   }

   if (equal(tok,"goto")){
       if (equal(tok->next, "*")){
           Node *node = new_node(ND_GOTO_EXPR, tok);
           node->lhs = expr(&tok, tok->next->next);
           *rest = skip(tok, ";");
           return node;  
       }
       Node *node = new_node(ND_GOTO, tok);
       node->label = get_ident(tok->next);
       node->goto_next= gotos;
       gotos = node;
       *rest = skip(tok->next->next, ";");
       return node;
   }

   if (equal(tok, "break")){
       if (!brk_label){
          error_tok(tok,"stray break");
       }
       Node *node = new_node(ND_GOTO, tok);
       node->unique_label = brk_label;
       *rest = skip(tok->next, ";");
       return node;
   }

   if (equal(tok, "continue")){
      if (!cont_label){
        error_tok(tok, "stray continue");
      }
      Node *node = new_node(ND_GOTO, tok);
      node->unique_label = cont_label;
      *rest = skip(tok->next, ";");
      return node;
   }

   if (tok->kind == TK_IDENT && equal(tok->next, ":")){
      Node *node = new_node(ND_LABEL, tok);
      node->label = strndup(tok->loc, tok->len);
      node->unique_label = new_unique_name();
      node->lhs = stmt(rest, tok->next->next);
      node->goto_next = labels;
      labels = node;
      return node;
   }
   
   if (equal(tok, "{")){
       return compound_stmt(rest, tok->next);
   }

   return expr_stmt(rest, tok);
}

// compound-stmt = (typedef | declaration | stmt)* "}"

static Node *compound_stmt(Token **rest, Token *tok){
   Node *node = new_node(ND_BLOCK, tok);
   Node head = {};
   Node *cur = &head;

   enter_scope();
   while (!equal(tok, "}")){
      if (is_typename(tok) && !equal(tok->next, ":")){
         VarAttr attr = {};
         Type *basety = declspec(&tok,tok, &attr);

         if (attr.is_typedef){
            tok = parse_typedef(tok, basety);
            continue;
         }

         if (is_function(tok)){
            tok = function(tok, basety, &attr);
            continue;
         }
         if (attr.is_extern){
            tok = global_variables(tok, basety, &attr);
            continue;
         }
         cur = cur->next = declaration(&tok, tok, basety, &attr);
      } else {
          cur = cur->next = stmt(&tok, tok);
      }

      add_type(cur);
   }

   leave_scope();

   node->body = head.next;
   *rest =  tok->next;
   return node;
}

// expr-stmt = expr? ";"
static Node *expr_stmt(Token **rest, Token *tok){
    if (equal(tok, ";")){
       *rest = tok->next;
       return new_node(ND_BLOCK, tok);
    }

    Node *node = new_node(ND_EXPR_STMT, tok);
    node->lhs = expr(&tok , tok);
    *rest = skip(tok, ";");
    return node;
}

// expr = assign (","  expr)?
static Node *expr(Token **rest, Token *tok){
    Node *node = assign(&tok, tok);

    if (equal(tok, ",")){
       return new_binary(ND_COMMA, node, expr(rest, tok->next), tok);
    }
    *rest = tok;
    return node;
}

static int64_t eval(Node *node){
   return eval2(node, NULL);
}

// evaluating node as a constant expression
static int64_t eval2(Node *node, char ***label){
   add_type(node);

   if (is_flonum(node->ty)){
      return eval_double(node);
   }

   switch(node->kind){
      case ND_ADD:
         return eval2(node->lhs, label) + eval(node->rhs);
      case ND_SUB:
         return eval2(node->lhs, label) - eval(node->rhs);
      case ND_MUL:
         return eval(node->lhs) * eval(node->rhs);
      case ND_DIV:
         if (node->ty->is_unsigned){
             return (uint64_t)eval(node->lhs) / eval(node->rhs);
         }
         return eval(node->lhs) / eval(node->rhs);
      case ND_NEG:
         return -eval(node->lhs);
      case ND_MOD:
         if (node->ty->is_unsigned){
            return (uint64_t)eval(node->lhs) % eval(node->rhs);
         }
         return eval(node->lhs) % eval(node->rhs);

      case ND_BITAND:
         return eval(node->lhs) & eval(node->rhs);
      case ND_BITOR:
         return eval(node->lhs) | eval(node->rhs);
      case ND_BITXOR:
         return eval(node->lhs) ^ eval(node->rhs);
      case ND_SHL:
         return eval(node->lhs) << eval(node->rhs);
      case ND_SHR:
         if(node->ty->is_unsigned && node->ty->size == 8){
            return (uint64_t)eval(node->lhs) >> eval(node->rhs);
         } 
         return eval(node->lhs) >> eval(node->rhs);
      case ND_EQ:
         return eval(node->lhs) == eval(node->rhs);
      case ND_NE:
         return eval(node->lhs) != eval(node->rhs);
      case ND_LT:
         if (node->lhs->ty->is_unsigned){
           return (uint64_t)eval(node->lhs) < eval(node->rhs);
         }
         return eval(node->lhs) < eval(node->rhs);

      case ND_LE:
         if (node->lhs->ty->is_unsigned){
            return (uint64_t)eval(node->lhs) <= eval(node->rhs);
         }
         return eval(node->lhs) <= eval(node->rhs);
      case ND_COND:
         return eval2(node->rhs, label);

      case ND_NOT:
         return !eval(node->lhs);
      case ND_BITNOT:
         return ~eval(node->lhs);
      case ND_LOGAND:
         return eval(node->lhs) && eval(node->rhs);
      case ND_LOGOR:
         return eval(node->lhs) || eval(node->rhs);
      case ND_CAST: {
         int64_t val = eval2(node->lhs, label);
         if (is_integer(node->ty)){
            switch(node->ty->size){
               case 1: return node->ty->is_unsigned ? (uint8_t)val : (int8_t)val;
               case 2: return node->ty->is_unsigned ? (uint16_t)val : (int16_t)val;
               case 4: return node->ty->is_unsigned ? (uint32_t)val : (int32_t)val;

                   
            }
         }
         return val;
      }
     
     case ND_ADDR:
         return eval_rval(node->lhs, label);
     case ND_LABEL_VAL:
         *label = &node->unique_label;
         return 0;

     case ND_MEMBER:
         if (!label){
             error_tok(node->tok, "not a compile-time constant");
         }
         if (node->tok,"invalid initializer"){
            error_tok(node->tok , "invalid initializer");

         }
         return eval_rval(node->lhs, label) + node->member->offset;

    case ND_VAR:
        if (!label){
           error_tok(node->tok, "not a compile-time constant");
        }
        if (node->var->ty->kind != TY_ARRAY && node->var->ty->kind != TY_FUNC){
           error_tok(node->tok, "invalid intializer");
        }
        *label = &node->var->name;
        return 0;

    case ND_NUM:
        return node->val;
 
   }
   error_tok(node->tok, "not a compile-time constant");
}


