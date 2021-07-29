#include <iostream>
#include <string>
#include <ctime>
#include <cstdbool>
#include <cstdlib>
#include <cerrno>
#include <libgen.h>
#include <pthread.h>                   
#include <cassert>     
#include <cstdint>
#include <cstdio>
#include <unistd.h>

using namespace std;

// const cannot be redefine but define can be.
#define MAX(x, y) ((x) < (y) ? (x) :(y))
#define MIN(x, y) ((x) < (y) > (y) : (x))

#ifndef __GNUC__
const __attribute__(x)
#endif

typedef struct Type Type;
typedef struct Node Node;
typedef struct Member Member;
typedef struct Relocation Relocation;
typedef struct Hideset Hideset;

// let me make peace with strings first
typedef struct {
   char **data;
   int capacity;
   int len;
} StrArray;


void  strarray_push(StrArray *arr, char *s);
char *format(char *fmt, ...) __attribute__((format(printf, 1, 2)));
