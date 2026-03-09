#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    FILE *file = fopen(argv[1], "r");
    
    char buf[128];
    char comma;
    while (!feof(file)) {
        int matches = fscanf(file, "%127[^ \n\t,] %1[,] ", buf, &comma);
        if (matches == 0) {
            printf("Parse error!\n");
            break;
        }
        printf("%d : ^%s$\n", matches, buf);
    }
    
    return 0;
}