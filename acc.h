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

typedef struct {
   char **data;
   int capacity;
   int len;
} StrArray;

void array_push(StrArray *arr, char *s);
char *render(char *fmt, ...) __attribute__((render(printf, 1,2)));

