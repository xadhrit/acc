/*
void *calloc(std::size_t num, std::size_t size):   Allocates memor
num => number of objects
size => size of each object

void *realloc(void *ptr, std::size_t new_size);
*/
#include "acc.h"

void strarray_push(StrArray *arr, char *s){
    if (!arr->data){
       arr->data = calloc(8, sizeof(char *));
       arr->capacity = 8;
    }

    if (arr->capacity == arr->len){
       arr->data = realloc(arr->data, sizeof(char *) * arr->capacity * 2);
       arr->capacity *= 2;
       for (int i = arr->len; i < arr->capacity; i++){
           arr->data[i] = NULL;
       }
    }
    arr->data[arr->len++] = s;
}
  
char *format(char *fmt, ...) {
    char *buf;
    size_t buflen;
    FILE *out = open_memstream(&buf, &buflen);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fclose(out);
    return buf;
}
