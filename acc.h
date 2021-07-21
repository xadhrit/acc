/*
Main Header file for Adhrit C Compiler
*/
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <glob.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdnoreturn.h>
#include <libgen.h>

#define MAX(x,y) ((x) < (y) ? (y) : (x) )
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#ifndef __GNUC__
#define __attribute__(x)
#endif

//essentail ds

typedef struct Type Type;
typedef struct Node Node;
typedef struct Member Member;
typedef struct Relocation Relocation;
typedef struct Hideset Hideset;

// strings.c

typedef struct {
   char **data;
   int capacity;
   int len;
} StrArray;

void array_push(StrArray *arr, char *s);
char *render(char *fmt, ...) __attribute__((format(printf, 1,2)));

// Tokens
typedef enum {
   TK_PUNCT, // Punctuators
   TK_IDENT, // Identation
   TK_KEYWORD, // Keywords
   TK_STR, //String literals
   TK_NUM, // Numeric literals
   TK_PP_NUM, // Preprocessing numbers
   TK_EOF, // End-of-line markers
} TokenKind;

typedef struct{
   char *name;
   int file_no;
   char *constants;

   // For #line directive
   char *display_name
   int line_delta;
} File;

// Token type
typedef struct Token Token;
struct Token{
    TokenKind kinf;  //Token Kind
    Token *next;   //Next Token
    int64_t val;   // If kind is TK_NUM , its value
    long double fval; //If kind is TK_NUM, its value
    char *loc; // TOken location
    int len;  // Token length
    Type *ty; //Used if TK_NUM or TK_STR
    char *str;  // String literal contents including

    File *file;  //Source location
    char *filename; //Filename
    int line_no; //Line number
    int line_delta; //Line number
    bool at_bol; // True if this token is at beginning of the line
    bool has_space; // True if this token follows a space
    Hideset *hideset; // FOr macro expansion
    Token *origin; //If this is expanded from a macro, the original token
};


noreturn void error(char *fmt, ...) __attribute__((format(printf, 1, 2)));
noreturn void error_at(char *loc, char *fmt , ...) __attribute__((format(printf,2,3)));
noreturn void error_tok(Token *tok, char *fmt, ...) __attribute__((format(printf, 2,3)));
void warn_tok(Token *tok, char *fmt, ...) __attribute__((format((printf,2,3))));
bool equal(Token *tok, char *op);
Token *skip(Token *tok, char *op);
bool consume(Token **rest, Token *tok, char *str);
void convert_pp_tokens(Token *tok);
File **get_input_files(void);
File *new_file(char *name, int file_no, char *contents);
Token *tokenize_string_literal(Token *tok, Type *basety);
Token *tokenize(File *file);
Token *tokenize_file(char *filename);

#define unreachable() \
    error("internal error at %s:%d", __FILE__, __LINE__);

// preprocessing declarations
char *search_include_paths(char *filename);
void init_macros(void);
void define_macro(char *name, char *buf);
void undef_macro(char *name);
Token *preprocess(Token *tok);

// parser

// Variables or function
typedef struct Obj Obj;
struct Obj {
   Obj *next;
   char *name;  // Variable name
   Type *ty;   //type
   Token *tok;  //representative token
   bool is_local;  // local or global/function
   int align;   //alignment

   // Local variable
   int offset;

   // Global variable or function
   bool is_function;
   bool is_definition;
   bool is_static;

   // global variable 
   bool is_tentative;
   bool is_tls;
   char *init_data;
   Relocation *rel;

   // function
   bool is_inline;
   Obj *params;
   Node *body;
   Obj *locals;
   Obj *va_area;
   Obj *alloca_bottom;
   int stack_size;

   // Static inline function
   bool is_live;
   bool is_root;
   StrType refs;
};

// global variable can be initiailized either by a constant experience or a pointer to another global variable. The struct representative latter.
typedef struct Relocation Relocation;
struct Relocation {
    Relocation *next;
    int offset;
    char **label;
    long addend;
};

// AST node
typedef enum {
   ND_NULL_EXPR, // Do nothing
   ND_ADD, // + 
   ND_SUB, // -
   ND_DIV, // /
   ND_MUL, // *
   ND_NEG, // unary -
   ND_MOD, // %
   ND_BITAND, // &
   ND_BITOR, // |
   ND_BITXOR, // ^
   ND_SHL, // <<
   ND_SHR, // >>
   ND_EQ, // ==
   ND_NE, // !=
   ND_LT, // <
   ND_GE, // >
   ND_LE, // != 
   ND_ASSIGN, // =
   ND_COND, // ?: 
   ND_MEMBER, // (struct member access)
   ND_ADDR,  // unary &
   ND_DEREF,  //unary *
   ND_NOT , // !
   ND_BITNOT, //~
   ND_LOGAND, // &&
   ND_RETURN , // "return"
   ND_IF, // "if"
   ND_FOR, // "for" or "while"
   ND_DO, // "DO"
   ND_SWITCH ,  // "switch"
   ND_CASE, // "CASE"
   ND_BLOCK, // {.....}
   ND_GOTO, // "goto"
   ND_GOTO_EXPR, // "goto" labels-as-values
   ND_LABEL, // labeled statement
   ND_LABEL_VAL,  // [GNU] Labels-as-value
   ND_FUNCALL, // Function call
   ND_EXPR_CALL, //Expression statement
   ND_STMT_EXPR, // Statement epxression
   ND_VAR, // Variable
   ND_VLA_PTR, // VLA designator
   ND_NUM, // Integer
   ND_CAST, // Type cast
   ND_MEMZERO, //Zero-Clear a staack vairable
   ND_ASM,  // "asm"
   ND_CAS, // Atomic compare and swap
   ND_EXCH, // Atomic exchange

} NodeKind;

// AST node type
struct Node {
   NodeKind kind;  // node kind
   Node *next; // Next Node
   Type *ty; // Type e.g. int or pointer to int
   Token *tok; // Representative token

   Node *lhs; //left-hand side
   Node *rhs; // right-hand-style

   // "if" or "for" statement
   Node *cond;
   Node *then;
   Node *els; 
   Node *init;
   Node *inc;

   // ("BREAK" and "CONTINUE").lowercase() labels
   char *brk_label;
   char *cont_label;

   // Block or statement expression
   Node *member;

   // Struct member access
   Member *member;

   // Function call
   Type *func_ty;
   Node *args;
   bool pass_by_stack;
   Obj *ret_buffer;

   // Goto or labeled statment, or labgels as values
   char *label;
   char *unique_label;
   Node *goto_next;

   // Switch
   Node *case_next;
   Node *default_case;

   //Case
   long begin;
   long end;

   // "asm" string literal
   char *asm_str;

   // Atomic comapre-and-swap
   Node *cas_addr;
   Node *cas_old;
   Node *cas_new;

   // Atomic op= operators;
   Obj *atomic_addr;
   Node *atomic_expr;

   // Variable
   Obj *var;

   // Numeric literal
   int64_t val;
   long double fval;

};

Node *new_cast(Node *expr, Type *ty);
int64_t const_expr(Token **rest, Token *tok);
Obj *parse(Token *tok);

// type.c

typedef enum {
   TY_VOID,
   TY_BOOL,
   TY_CHAR,
   TY_SHORT,
   TY_INT,
   TY_LONG,
   TY_FLOAT,
   TY_DOUBLE,
   TY_ENUM,
   TY_LDOUBLE,
   TY_ENUM,
   TY_PTR,
   TY_FUNC,
   TY_ARRAY,                            
   TY_VAL, // variable-length array
   TY_STRUCTM
   TY_UNION,
} TypeKind;


struct Type {
   TypeKind kind; 
   int align  // aligment
   bool is_unsigned  ; // unsigend or signed 
   int size; // sizeof() value
   bool  is_atomic; // true if _Atomic
   Type *origin; // for type compatiability check

   //
   Type *base;

   // Declaration
   Token *name;
   Token *name_pos;

   // Array
   int array_len;

   // Variable-length array
   Node *vla_len; // # of elements
   Obj *vla_size; // sizeof() value

   // Struct
   Member *members;
   bool is_flexible;
   bool is_packed;

   // Function type
   Type *return_ty;
   Type *params;
   bool is_variadic;
   Type *next;
};

// Struct Number
struct Member {
   Member *next;
   Type *ty;
   Token *tok; // for error message
   Token *name;
   int idx;
   int align;
   int offset;

   // Bitfield
   bool is_bitfield;
   int bit_offset;
   int bit_width;

};

extern Type *ty_void;
extern Type *ty_bool;

extern Type *ty_char;
extern Type *ty_short;
extern Type *ty_int;
extern Type *ty_long;

extern Type *ty_uchar;
extern Type *ty_ushort;
extern Type *ty_uint;
extern Type *ty_ulong;

extern Type *ty_uchar;
extern Type *ty_ushort;
extern Type *ty_uint;
extern Type *ty_ulong;

extern Type *ty_float;
extern Type *ty_double;
extern Type *ty_ldouble;

bool is_integer(Type *ty);
bool is_flonum(Type *ty);
bool is_numeric(Type *ty);
bool is_compatible(Type *t1, Type *t2);
Type *copy_type(Type *ty);
Type *pointer_to(Type *base);
Type *func_type(Type *return_ty);
Type *array_of(Type *base, int size);
Type *vla_of(Type *base, Node *epxr);
Type *enum_type(void);
Type *struct_type(void);
void add_type(Node *node);

// codegen.c

void codegen(Obj *prog, FILE *out);
int align_to(int n , int align);

// unicode.c

int encode_utf8(char *buf, uint32_t c){
   uint32_t decode_utf8(char **new_pos, char *p);
   bool is_ident1(uint32_t c);
   bool is_ident2(uint32_t c);
   int display_width(char *p, int len);
}
 
// hashmap.c

typedef struct {
   char *key;
   int keylen;
   void *val;
} HashEntry;

typedef struct{
   HashEntry *buckets;
   int capacity;
   int used;
} HashMap;

void *hashmap_get(HashMap *map, char *key);
void *hashmap_get2(HashMap *map, char *key, int keylen);
void hashmap_put(HashMap *map, char *key, void *val);
void hashmap_put2(HashMap *map, char *key, int keylen, void *val);
void hashmap_delete(HashMap *map, char *key);
void hashmap_delete2(HashMap *map, char *key, int keylen);
void hashmap_test(void);

// main.c

bool file_exists(char *path);

extern StrArray include_paths;
extern bool opt_fpic;
extern bool opt_fcommon;
extern char *base_file;
