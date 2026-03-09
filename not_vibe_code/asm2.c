#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum event_type {
    SYMBOL,
    LABEL_DEF,
    LIST_ITEM,
    LIST_END,
    ENDFILE,
    ERROR
};

typedef struct {
    char buf[512];
    int is_label_def, has_comma, is_end_of_list, eof, error;
    
    FILE *file;
} assembler_ctx_t;

assembler_ctx_t *new_assembler(char *fname) {
    FILE *file = fopen(fname, "r");
    if (file == NULL) return NULL;
    
    assembler_ctx_t *result = calloc(1, sizeof(assembler_ctx_t));
    result->file = file;
    
    return result;
}

void delete_assembler(assembler_ctx_t *ctx) {
    fclose(ctx->file);
    free(ctx);
}

int read_symbol(assembler_ctx_t *ctx) {
    if (feof(ctx->file)) {
        ctx->eof = 1;
        return -1;
    }
    
    char comma;
    int matches = fscanf(ctx->file, "%511[^ \n\t,] %1[,] ", ctx->buf, &comma);
    if (ferror(ctx->file) || matches == 0) {
        ctx->error = 1;
        return -1;
    }
    
    ctx->is_end_of_list = ctx->has_comma;
    ctx->has_comma = (matches > 1);
    ctx->is_label_def = (ctx->buf[strlen(ctx->buf) - 1] == ':');
    
    if ((ctx->has_comma || ctx->is_end_of_list) && ctx->is_label_def) {
        ctx->error = 1;
        return -1;
    }
    
    return 0;
}

enum event_type get_symbol_type(assembler_ctx_t *ctx) {
    enum event_type result =  ctx->error          ? ERROR
                            : ctx->eof            ? ENDFILE
                            : ctx->is_label_def   ? LABEL_DEF
                            : ctx->has_comma      ? LIST_ITEM
                            : ctx->is_end_of_list ? LIST_END
                            :                       SYMBOL
    ;
    
    return result;
}

int main(int argc, char *argv[]) {
    assembler_ctx_t *assembler = new_assembler(argv[1]);
    
    while (!read_symbol(assembler)) {
        printf("%-16s is ", assembler->buf);
        switch (get_symbol_type(assembler)) {
            case ERROR:     printf("ERROR"); break;
            case ENDFILE:   printf("ENDFILE"); break;
            case LABEL_DEF: printf("LABEL_DEF"); break;
            case LIST_ITEM: printf("LIST_ITEM"); break;
            case LIST_END:  printf("LIST_END"); break;
            case SYMBOL:    printf("SYMBOL"); break;
        }
        printf("\n");
    }
    
    printf(
        "Ended on %s\n", 
          assembler->eof   ? "EOF"
        : assembler->error ? "error"
        :                    "impossible"
    );
    
    delete_assembler(assembler);
    return 0;
}