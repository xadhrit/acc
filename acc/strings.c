#include "acc.h"

void array_push(StrArray *arr, char *s ){
   if (!arr->data){
      arr->data = calloc(8, sizeof(char *));
      arr->capacity = 8;
   }

   if (arr->capacity == arr->len){
      arr->data = realloc(arr->data, sizeof(char *) * arr->capacity * 2 );
      arr->capacity *= 2;
      for (int i = arr->len; i < arr->capacity; i++){
        arr->data[i] = NULL;
      }
   }

   arr->data[arr->len++]  =s;
}

char *render(char *fmt, ...){
   char *buf;
   size_t buflen;
   FILE *fp =  open_memstream(&buf, &buflen);

   va_list list;
   va_start(list, fmt);
   vfprintf(fp, fmt, list);
   va_end(list);
   fclose(fp);
   return buf;
}

