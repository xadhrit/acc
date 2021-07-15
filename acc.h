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


 
