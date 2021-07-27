#include "acc.h"

#define GP_MAX 6
#define FP_MAX 8

static FILE *out_file;
static int depth;
static char *argreg8[] = {"%dil", "%sil", "%dl", "%cl", "%r8b","%r9b"};
static char *argreg16[] = {"%di", "%si", "%dx", "%cx", "%r8w","%r9w"};
static char *argreg32[] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
static char *argreg64[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
static Obj *current_fn;

static void gen_expr(Node *node);
static void gen_stmt(Node *node);

__attribute__((format(printf, 1, 2)))
static void println(char *fmt, ...){
    va_list op;
    va_start(ap, fmt);
    vfprintf(out_file, fmt, ap);
    va_end(ap);
    fprintf(out_file, "\n");
}

static int count(void){
    static int i = 1;
    return i++;
}

static void push(void){
    println(" push%%rax ");
    depth++;
}

static void pop(char *arg){
   println(" pop %s ", arg);
   depth--;
}

static void pushf(void){
   println(" sub $8, %%rsp ");
   println(" movsd %%xmm0, (%%rsp) ");
   depth++;
}

static void popf(int reg){
   println(" movsd (%%rsp), %%xmm%d", reg );
   println(" add $8, %%rsp ");
   depth--;
}

/* Round up `n` to nearest multiple of `align`.
 e.g.: align_to(12, 19) returns 19
    align_to(2, 5) return 5
    align_to(11, 5) return 15
*/
int align_to(int n, int align){
   return (n + align - 1) / align * align;
}

static char *reg_dx(int sz){
   switch(sz){
      case 1: return "%dl";
      case 2: return "%dx";
      case 4: return "%edx";
      case 8: return "%rdx";
   }
   unreachable();
}

static char *reg_ax(int sz){
   switch(sz){
      case 1: return "%al";
      case 2: return "%ax";
      case 4: return "%eax";
      case 8: return "%rax";
   }
   unreachable();
}

// how to calculate absolute addr of a given node.
static void gen_addr(Node *node){
    switch(node->kind){
       case ND_VAR:
           if (node->var->ty->kind  == TY_VLA){
               println(" mov %d(%%rbp), %%rax ", node->var->offset  );
               return;
           }
           if (opt_fpic){
               if (node->var->is_tls){
                  println(" data16 lea %s@tlsgd(%%rip), %%rdi ", node->var->name);
                  println(" .value 0x6666" );
                  println(" rex64 ");
                  println(" call __tls get_addr@PLT ");
                  return;
               }

               println(" mov %s@GOTPCREL (%%rip), %%rax", node->var->name);
               return;
           }
           
           // Thread-local variable
           if (node->var->is_tls){
              println(" mov %%fs:0 , %%rax");
              println(" add $%s@tpoff, %%rax ", node->var->name);
              return;
           }

           // function
           if (node->ty->kind == TY_FUNC){
              if (node->var->is_definition){
                 println(" lea %s(%%rip), %%rax ", node->var->name);
              }
              else {
                 println(" mov %s@GOTPCREL(%%rip), %%rax ", node->var->name);
              }
              return;
           }

           println(" lea %s(%%rip), %%rax", node->var->name);
           return;

       case ND_DEREF:
            gen_expr(node->lhs);
            return;
       case ND_COMMA:
            gen_expr(node->lhs);
            gen_addr(node->rhs);
            return;
       case ND_MEMBER:
            gen_addr(node->lhs);
            println(" add $%d, %%rax", node->member->offset);
            return;
       case ND_FUNCALL:
            if (node->ret_buffer){
                gen_expr(node);
                return;
            }
            break;
       case ND_ASSIGN:
       case ND_COND: 
            if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION){
                gen_expr(node);
                return;
            }

            break;
       case ND_VLA_PTR:
            println(" lea %d(%%rbp), %%rax ", node->var->offset);
            return;
    }

    error_tok(node->tok, "not an lvalue");
}

// load a value from where %rax is pointing to
static void load(Type *ty){
   switch(ty->kind){
      case TY_ARRAY:
      case TY_STRUCT:
      case TY_UNION:
      case TY_FUNC:
      case TY_VLA:
           return;
      case TY_FLOAT:
           println("  movss (%%rax), %%xmm0 ");
           return;
      case TY_DOUBLE:
           println("  movsd (%%rax), %%xmm0 ");
           return;
      case TY_LDOUBLE:
           println(" fldt (%%rax)");
           return;
   }

   char *insn = ty->is_unsigned ? "movz" : "movs";

   if (ty->size == 1){
       println(" %sbl (%%rax), %%eax ", insn);
   }
   else if (ty->size == 2){
      println(" %swl (%%rax), %%eax", insn);
   }
   else if (ty->size == 4){
      println(" movsxd (%%rax), %%rax ");
   }
   else {
      println(" mov (%%rax), %%rax ");
   }
}

// store %rax to an address that stack top is pointing to
static void store(Type *ty){
   pop("%rdi");

   switch(ty->kind){
      case TY_STRUCT:
      case TY_UNION:
         for (int i=0; i < ty->size; i++){
             println(" mov %d(%%rax), %%r8b ", i);
             println(" mov %%r8b, %d(%%rdi) ", i);
         }
         return;
      case TY_FLOAT:
           println(" movss %%xmm0, (%%rdi)");
           return;
      case TY_DOBULE:
           println(" movsd %%xmm0, (%%rdi) ");
           return;
      case TY_LDOUBLE:
           println(" fstpt (%%rdi) ");
           return;
   }

   if (ty->size == 1){
      println(" mov %%al, (%%rdi) ");
   }
   else if (ty->size == 2){
      println(" mov %%ax, (%%rdi) ");
   }
   else if (ty->size == 4){
      println(" mov %%eax, (%%rdi) ");
   }
   else {
      println(" mov %%rax, (%%rdi) ");
   }
}

static void cmp_zero(Type *ty){
   switch(ty->kind){
      case TY_FLOAT:
           println(" xorps %%xmm1, %%xmm1 ");
           println(" ucomiss %%xmm1, %%xmm0  ");
           return;
      case TY_DOUBLE:
           println(" xorpd %%xmm1, %%xmm1 ");
           println(" ucomisd %%xmm1, %%xmm0 ");
           return;
      case TY_LDOUBLE:
           println(" fldz ");
           println(" fucomip ");
           println(" fstp %%st(0)  ");
           return;
   }

   if (is_integer(ty) && ty->size <= 4){
      println(" cmp $0, %%eax ");
   }
   else {
      println(" cmp $0, %%rax ");
   }
}

enum { I8, I16, I32 , I64, U8, U32, U64, F32, F64, F80 };

static int getTypeId(Type *ty){
   switch(ty->kind ){
     case TY_CHAR:
          return ty->is_unsigned ? U8 :I8;

     case TY_SHORT:
          return ty->is_unsigned ? U16 : I16;
     case TY_INT:
          return ty->is_unsigned ? U32 : I32;
     case TY_LONG:
          return ty->is_unsigned ? U64 : I64;
     case TY_FLOAT:
          return F32;
     case TY_DOUBLE:
          return F64;
     case TY_LDOUBLE:
          return F80;
   }
   return U64;
}

// type casting table
static char i32i8[] = "movsbl %al, %eax";
static char i32u8[] = "movzbl %al, %eax";
static char i32i16[] = "movswl %ax, %eax";
static char i32u16[] = "movzwl %ax, %eax";
static char i32f32[] = "cvtsi2ssl %eax, %xmm0 ";
static char i32i64[] = "movsxd %eax, %rax";



