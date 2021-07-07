#include "acc.h"

typedef enum {
   FILE_NONE, FILE_C, FILE_ASM, FILE_OBJ, FILE_AR, FILE_DSO,
}FileType;

StrArray include_paths;
bool opt_fcommon = true;
bool opt_fpic;

static FileType opt_x;
static StrArray opt_include;
static bool opt_E;
static bool opt_M;
static bool opt_MD;
static bool opt_MMD;
static bool opt_MP;
static bool opt_S;
static bool opt_c;
static bool opt_cc1;
static bool opt_hash_hash_hash;
static bool opt_shared;
static bool opt_static;
static char *opt_MF;
static char *opt_MT;
static char *opt_o;

static StrArray ld_extra_args;
static StrArray std_include_paths;

char *base_file;
static char *output_file;

static StrArray input_paths;
static StrArray tmpfiles;

static void usage(int status){
  fprintf(stderr, "acc [-o path ] <file>\n");
  exit(status);
}

static bool take_arg(char *arg){
  char *x[] = {
    "-o", "-I", "-idirafter","-include", "-x", "-MF","-MT", "-Xlinker",
  };

  for (int i=0; i< sizeof(x) / sizeof(*x); i++){
     if (!strcmp(arg, x[i])){
       return true;
     }
  }
  return false;
}

static void add_default_include_paths(char *argv0){
    array_push(&include_paths, render("%s/include", dirname(strdup(argv0))));
    array_push(&include_paths, "/usr/local/include");
    array_push(&include_paths,"/use/include/x86_64-linux-gnu");
    array_push(&include_paths,"/usr/include");

    //
    for (int i=0; i< include_paths.len ; i++){
      array_push(&std_include_paths, include_paths.data[i]);
    }
}




static FileType parse_opt_x(char *s){
  if (!strcmp(s, "c")){
    return FILE_C;
  }
  if (!strcmp(s, "assembler")){
     return FILE_ASM;
  }
  if (!strcmp(s, "none")){
     return FILE_NONE;
  }
  error("<command line>: unknown argument for -x : %s", s);
}

static char *quote_makefile(char *s){
   char *buf = calloc(1, strlen(s) * 2 + 1);

   for (int i=0, j=0; s[i]; i++){
      switch(s[i]){
         case '$':
           buf[i++] = '$';
           buf[i++] = '$';
           break;

         case '#':
           buf[j++] = '\\';
           buf[j++] = '#';
           break;

         case ' ':
         case '\t':
            for (int k = i-1; k >= 0 && s[k] == '\\'; k--){
              buf[j++]='\\';
            }
            buf[j++] = '\\';
            buf[j++] = s[i];
            break;

        default:
            buf[j++] = s[i];
            break;
      }
   }
   return buf;
}


static void parse_args(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    if (take_arg(argv[i]))
      if (!argv[++i])
        usage(1);

  StrArray idirafter = {};

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-###")) {
      opt_hash_hash_hash = true;
      continue;
    }

    if (!strcmp(argv[i], "-cc1")) {
      opt_cc1 = true;
      continue;
    }

    if (!strcmp(argv[i], "--help"))
      usage(0);

    if (!strcmp(argv[i], "-o")) {
      opt_o = argv[++i];
      continue;
    }

    if (!strncmp(argv[i], "-o", 2)) {
      opt_o = argv[i] + 2;
      continue;
    }

    if (!strcmp(argv[i], "-S")) {
      opt_S = true;
      continue;
    }

    if (!strcmp(argv[i], "-fcommon")) {
      opt_fcommon = true;
      continue;
    }

    if (!strcmp(argv[i], "-fno-common")) {
      opt_fcommon = false;
      continue;
    }

    if (!strcmp(argv[i], "-c")) {
      opt_c = true;
      continue;
    }

    if (!strcmp(argv[i], "-E")) {
      opt_E = true;
      continue;
    }

    if (!strncmp(argv[i], "-I", 2)) {
      array_push(&include_paths, argv[i] + 2);
      continue;
    }


    if (!strcmp(argv[i], "-include")) {
      array_push(&opt_include, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-x")) {
      opt_x = parse_opt_x(argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-x", 2)) {
      opt_x = parse_opt_x(argv[i] + 2);
      continue;
    }

    if (!strncmp(argv[i], "-l", 2) || !strncmp(argv[i], "-Wl,", 4)) {
      array_push(&input_paths, argv[i]);
      continue;
    }

    if (!strcmp(argv[i], "-Xlinker")) {
      array_push(&ld_extra_args, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-s")) {
      array_push(&ld_extra_args, "-s");
      continue;
    }

    if (!strcmp(argv[i], "-M")) {
      opt_M = true;
      continue;
    }

    if (!strcmp(argv[i], "-MF")) {
      opt_MF = argv[++i];
      continue;
    }

    if (!strcmp(argv[i], "-MP")) {
      opt_MP = true;
      continue;
    }

    if (!strcmp(argv[i], "-MT")) {
      if (opt_MT == NULL)
        opt_MT = argv[++i];
      else
        opt_MT = render("%s %s", opt_MT, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-MD")) {
      opt_MD = true;
      continue;
    }

    if (!strcmp(argv[i], "-MQ")) {
      if (opt_MT == NULL){
        opt_MT = quote_makefile(argv[++i]);
      }
      else{
        opt_MT = render("%s %s", opt_MT, quote_makefile(argv[++i]));
      }
      continue;
    }

    if (!strcmp(argv[i], "-MMD")) {
      opt_MD = opt_MMD = true;
      continue;
    }

    if (!strcmp(argv[i], "-fpic") || !strcmp(argv[i], "-fPIC")) {
      opt_fpic = true;
      continue;
    }

    if (!strcmp(argv[i], "-cc1-input")) {
      base_file = argv[++i];
      continue;
    }

    if (!strcmp(argv[i], "-cc1-output")) {
      output_file = argv[++i];
      continue;
    }

    if (!strcmp(argv[i], "-idirafter")) {
      array_push(&idirafter, argv[i++]);
      continue;
    }

    if (!strcmp(argv[i], "-static")) {
      opt_static = true;
      array_push(&ld_extra_args, "-static");
      continue;
    }

    if (!strcmp(argv[i], "-shared")) {
      opt_shared = true;
      array_push(&ld_extra_args, "-shared");
      continue;
    }

    if (!strcmp(argv[i], "-L")) {
      array_push(&ld_extra_args, "-L");
      array_push(&ld_extra_args, argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-L", 2)) {
      array_push(&ld_extra_args, "-L");
      array_push(&ld_extra_args, argv[i] + 2);
      continue;
    }

    /*if (!strcmp(argv[i], "-hashmap-test")) {
      hashmap_test();
      exit(0);
    }*/

    // These options are ignored for now.
    if (!strncmp(argv[i], "-O", 2) ||
        !strncmp(argv[i], "-W", 2) ||
        !strncmp(argv[i], "-g", 2) ||
        !strncmp(argv[i], "-std=", 5) ||
        !strcmp(argv[i], "-ffreestanding") ||
        !strcmp(argv[i], "-fno-builtin") ||
        !strcmp(argv[i], "-fno-omit-frame-pointer") ||
        !strcmp(argv[i], "-fno-stack-protector") ||
        !strcmp(argv[i], "-fno-strict-aliasing") ||
        !strcmp(argv[i], "-m64") ||
        !strcmp(argv[i], "-mno-red-zone") ||
        !strcmp(argv[i], "-w"))
      continue;

    if (argv[i][0] == '-' && argv[i][1] != '\0')
      error("unknown argument: %s", argv[i]);

    array_push(&input_paths, argv[i]);
  }

  for (int i = 0; i < idirafter.len; i++)
    array_push(&include_paths, idirafter.data[i]);

  if (input_paths.len == 0)
    error("no input files");

  // -E implies that the input is the C macro language.
  if (opt_E)
    opt_x = FILE_C;
}

static FILE *open_file(char *path) {
  if (!path || strcmp(path, "-") == 0){
      return stdout;
  }

  FILE *out = fopen(path, "w");
  if (!out){
     error("cannot open output file : %s %s ", path, strerror(errno));
  }
  return out;

}

static bool endsWith(char *p, char *q){
   int len1 = strlen(p);
   int len2 = strlen(q);
   return (len1 >= len2 ) && !strcmp(p +len1 - len2,  q );
}

static char *replace_extn(char *tmpl, char *extn){
    char *filename = basename(strdup(tmpl));
    char *dot = strchr(filename, ".");
    if (dot){
      *dot = '\0';
    }
    return render("%s%s", filename, extn);
}

static void cleanup(void){
  for (int i = 0; i < tmpfiles.len; i++){
     unlink(tmpfiles.data[i]);
  }
}

static char *create_tmpfile(void){
   char *path = strdup("/tmp/acc-XXXXXX");
   int p = mkstemp(path);
   if (p == -1){
      error("mkstemp failed : %s", strerror(errno));
   }
   close(p);

   array_push(&tmpfiles , path);
   return path;
}

static void run_subprocess(char **argv){
  //if -### is given , dump the subprocess's 
  if (opt_hash_hash_hash){
     fprintf(stderr, "%s", argv[0]);
     for (int i=1; argv[i];i++)   {
       fprintf(stderr, "%s", argv[i]);
     } 
     fprintf(stderr, "\n");
  }
  if (fork() == 0){
     //Child Process. Run a new command
     execvp(argv[0], argv);
     fprintf(stderr, "exec failed: %s : %s\n", argv[0], strerror(errno));
     _exit(1);
  }

  // wait for the child process to finish
  int status;
  while (wait(&status) > 0);
  if (status != 0){
      exit(1);
  }
}

static void assemble(char *input, char * output){
   char *cmd[] = {"as", "-c", input, "-o", output, NULL};
   run_subprocess(cmd);
}

static void run_cc1(int argc, char **argv, char *input, char *output){
   char **args = calloc(argc + 10, sizeof(char *));
   memcpy(args, argv, argc * sizeof(char *));
   args[argc++] = "-cc1";

   if (input){
     args[argc++] = "-cc1-input";
     args[argc++] = input;
   }

   if (output){
     args[argc++] = "-cc1-output";
     args[argc++] = output;
   }
   run_subprocess(args);
}

// check if file exists or not
//1. find the file 
static char *find_file(char *pattern){
   char *path = NULL;
   glob_t buf  = {};
   glob(pattern, 0, NULL, &buf);
   if (buf.gl_pathc >0){
      path = strdup(buf.gl_pathv[buf.gl_pathc -1]);
   }
   globfree(&buf);
   return path;
}

bool file_exists(char *path){
  struct stat st;
  return !stat(path, &st);
}

static char *find_libpath(void){
  if (file_exists("/usr/lib/x86_64-linux-gnu/crti.o")){
    return "/usr/lib/x86_64-linux-gnu";
  }
  if (file_exists("/usr/lib64/crti.o")){
    return "/usr/lib64";
  }
  error("library path is not found");
}

static char *find_gcc_libpath(void){
   char *paths[] = {
      "/usr/lib/gcc/x86_64-linux-gnu/*/crtbegin.o",
      "/usr/lib/gcc/x86_64-pc-linux-gnu/*/crtbegin.o",
      "usr/lob/gcc/x86_64-redhat-linux/*/crtbegin.o",
   };
   error("gcc library path is not found");
}

static void run_linker(StrArray *inputs, char *output){
    StrArray arr = {};

    array_push(&arr, "ld");
    array_push(&arr, "-o");
    array_push(&arr, output);
    array_push(&arr, "-m");
    array_push(&arr, "elf_x86_64");

    char *libpath = find_libpath();
    char *gcc_libpath = find_gcc_libpath();

    if (opt_shared){
       array_push(&arr,render("%s/crti.o", libpath));
       array_push(&arr, render("%s/crtbeginS.o", gcc_libpath));
    }
    else {
       array_push(&arr, render("%s/crt1.o", libpath));
       array_push(&arr, render("%s/crti.o", libpath));
       array_push(&arr, render("%s/crtbegin.o", gcc_libpath));
    }

    array_push(&arr, render("-L%s", gcc_libpath));
    array_push(&arr, "-L/usr/lib/x86_64-linux-gnu");
    array_push(&arr, "-L/usr/lib64");
    array_push(&arr, "-L/lib64");
    array_push(&arr, "-L/usr/lib/x86_64-linux-gnu");
    array_push(&arr, "-L/usr/lib/x86_64-pc-linux-gnu");
    array_push(&arr, "-L/usr/lib/x86_64-redhat-linux");
    array_push(&arr, "-L/usr/lib");
    array_push(&arr, "-L/lib");

    if (!opt_static){
       array_push(&arr, "-dynamic-linker");
       array_push(&arr, "/lib64/lib64-x86-64.so.2");
    }

    for (int i=0; i< ld_extra_args.len;i++){
       array_push(&arr, ld_extra_args.data[i]);
    }

    for (int i=0; i< inputs->len ; i++){
      array_push(&arr, inputs->data[i]);
    }

    if (opt_static){
       array_push(&arr, "--start-group");
       array_push(&arr, "-lgcc");
       array_push(&arr, "-lgcc_eh");
       array_push(&arr, "-lc");
       array_push(&arr, "--end-group");
    }
    else {
      array_push(&arr, "-lc");
      array_push(&arr, "-lgcc");
      array_push(&arr, "--as-needed");
      array_push(&arr, "-lgcc_s");
      array_push(&arr, "--no-as-needed");
    }

    if (opt_shared){
      array_push(&arr,render("%s/crtendS.o",gcc_libpath));
    }
    else {
      array_push(&arr, render("%s/crtend.o", gcc_libpath));
    }

    array_push(&arr , render("%s/crtn.o", libpath));
    array_push(&arr, NULL);

    run_subprocess(arr.data);
}

static FileType get_file_type(char *filename){
  if (opt_x != FILE_NONE){
    return opt_x;
  }

  if (endsWith(filename, ".a")){
    return FILE_AR;
  }
  if (endsWith(filename, ".so")){
    return FILE_DSO;
  }
  if (endsWith(filename, ".o")){
    return FILE_OBJ;
  }
  if (endsWith(filename, ".c")){
    return FILE_C;
  }
  if (endsWith(filename, ".s")){
    return FILE_ASM;
  }

  error("<command line>: uknown file extension");
}


int main( int argc , char **argv ){
    atexit(cleanup);
    parse_args(argc, argv);
                                                         
    if (input_paths.len > 1 && opt_o && (opt_c || opt_S | opt_E)){
       error("cannot specify '-o' with '-c', '-s' or '-E' with multiplying");
    }
    StrArray ld_args = {};

    for (int i=0; i< input_paths.len; i++){
      char *input = input_paths.data[i];
      
      if (!strncmp(input, "-Wl", 4)){
        char *s = strdup(input + 4);
        char *arg = strtok(s, ",");
        while (arg){
           array_push(&ld_args, arg);
           arg = strtok(NULL, ",");
        }
        continue;
      }
      char *output;
      if (opt_o){
         output = opt_o;
      }
      else if (opt_S){
        output = replace_extn(input, ".s");
      }
      else {
         output = replace_extn(input, ".o");
      }

      FileType type = get_file_type(input);

      if (type == FILE_OBJ || type == FILE_AR || type == FILE_DSO){
         array_push(&ld_args, input);
         continue;
      }

      if (type == FILE_ASM){
        if (!opt_S){
          assemble(input, output);
        }
        continue;
      }

      assert(type == FILE_C);

     
      if (opt_E || opt_M){
         run_cc1(argc, argv, input, NULL);
         continue;
      }

      if (opt_S){
         run_cc1(argc, argv, input, output);
         continue;
      }

      if (opt_c){
         char *tmp = create_tmpfile();
         run_cc1(argc, argv, input, tmp);
         assemble(tmp, output);
         continue;

      }
    
      //Compile , assemble and link
      char *tmp1 = create_tmpfile();
      char *tmp2 = create_tmpfile();
      run_cc1(argc, argv, input, tmp1);
      assemble(tmp1, tmp2);
      array_push(&ld_args, tmp2);
      continue;
    }

    if (ld_args.len > 0){
       run_linker(&ld_args, opt_o ? opt_o : "main.out");
    }
    return 0;


}
