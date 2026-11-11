/* args.c — print argc/argv and environment (1.0.80) */
#include "libc.h"

int main(int argc, char **argv) {
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("argv[%d]=", i);
        if (argv[i])
            puts(argv[i]);
        else
            puts("(null)");
    }
    printf("environ:\n");
    if (environ) {
        for (int i = 0; environ[i]; i++) {
            puts(environ[i]);
        }
    }
    char *foo = getenv("FOO");
    if (foo) {
        printf("getenv(FOO)=");
        puts(foo);
    } else {
        puts("getenv(FOO)=(null)");
    }
    return 0;
}
