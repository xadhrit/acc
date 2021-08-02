#!/bin/bash -e

echo -e  "Check if running\n"
gcc type.c -o type.o
gcc strings.c -o strings.o
gcc preprocessing.c -o preprocessing.o
gcc codgenerator.c -o codegenerator.o
gcc unicode.c -o unicode.o
gcc tokenize.c -o tokenize
gcc parser.c -o parser.o
gcc hashmap.c -o hashmap.o
gcc main.c -o main.o


