#pragma once
#ifndef GETOPT_H
#define GETOPT_H
extern int opterr; // if error message should be printed /
extern int optind; // index into parent argv vector /
extern int optopt; // character checked for validity /
extern int optreset; // reset getopt /
extern char *optarg; // argument associated with option /

#define no_argument       0
#define required_argument 1
#define optional_argument 2

struct option {
    const char* name;
    int         has_arg;
    int* flag;
    int         val;
};

int getopt(int nargc, char* const nargv[], const char* ostr);
int getopt_long(int, char**, char*, struct option*, int*);
#endif
