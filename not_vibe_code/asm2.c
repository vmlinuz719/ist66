#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LABEL_LEN 12

enum event_type {
    SYMBOL,
    LABEL_DEF,
    LIST_ITEM,
    LIST_END,
    ENDFILE,
    ERROR
};

typedef struct {
    char label[MAX_LABEL_LEN + 1];
    uint64_t value;
} label_def_t;

typedef struct {
    label_def_t *labels;
    int num_labels, max_labels;
} label_tab_t;

int insert_label_def(label_tab_t *ltab, char *label, uint64_t value) {
    if (ltab->num_labels == ltab->max_labels) return -1;
    
    strncpy(ltab->labels[ltab->num_labels].label, label, MAX_LABEL_LEN);
    ltab->labels[ltab->num_labels].label[MAX_LABEL_LEN] = 0;
    ltab->labels[ltab->num_labels].value = value;
    
    return ltab->num_labels++;
}

typedef struct {
    char label[MAX_LABEL_LEN + 1];
    uint64_t address;
    int is_relative, width, left_shift;
} label_thunk_t;

typedef struct {
    label_thunk_t *thunks;
    int num_thunks, max_thunks;
} thunk_tab_t;

label_tab_t *new_label_tab(int max_labels) {
    label_def_t *labels = malloc(sizeof(label_def_t) * max_labels);
    label_tab_t *result = malloc(sizeof(label_tab_t));
    result->labels = labels;
    result->num_labels = 0;
    result->max_labels = max_labels;
    return result;
}

void delete_label_tab(label_tab_t *ltab) {
    free(ltab->labels);
    free(ltab);
}

thunk_tab_t *new_thunk_tab(int max_thunks) {
    label_thunk_t *thunks = malloc(sizeof(label_thunk_t) * max_thunks);
    thunk_tab_t *result = malloc(sizeof(thunk_tab_t));
    result->thunks = thunks;
    result->num_thunks = 0;
    result->max_thunks = max_thunks;
    return result;
}

void delete_thunk_tab(thunk_tab_t *ttab) {
    free(ttab->thunks);
    free(ttab);
}

int insert_thunk(
    thunk_tab_t *ttab,
    char *label,
    uint64_t address,
    int is_relative,
    int width,
    int left_shift
) {
    if (ttab->num_thunks == ttab->max_thunks) return -1;
    
    strncpy(ttab->thunks[ttab->num_thunks].label, label, MAX_LABEL_LEN);
    ttab->thunks[ttab->num_thunks].label[MAX_LABEL_LEN] = 0;
    
    ttab->thunks[ttab->num_thunks].address = address;
    ttab->thunks[ttab->num_thunks].is_relative = is_relative;
    ttab->thunks[ttab->num_thunks].width = width;
    ttab->thunks[ttab->num_thunks].left_shift = left_shift;
    
    return ttab->num_thunks++;
}

int remove_thunk(thunk_tab_t *ttab, int index) {
    if (ttab->num_thunks == 0 || index < 0 || index >= ttab->num_thunks)
        return -1;
    
    ttab->thunks[index] = ttab->thunks[--ttab->num_thunks];
    return 0;
}

int get_label(label_tab_t *ltab, char *label) {
    for (int i = 0; i < ltab->num_labels; i++) {
        if (!strcmp(label, ltab->labels[i].label)) return i;
    }
    
    return -1;
}

int register_label_do_thunks(
    label_tab_t *ltab, char *label, uint64_t value,
    thunk_tab_t *ttab, uint64_t *work_area
) {
    if (insert_label_def(ltab, label, value) == -1) return -1;
    
    int thunks_done = 0;
    int current_thunk = 0;
    while (current_thunk < ttab->num_thunks) {
        if (strcmp(label, ttab->thunks[current_thunk].label)) {
            current_thunk++;
            continue;
        }
        
        label_thunk_t *thunk = &ttab->thunks[current_thunk];
        uint64_t v = thunk->is_relative ? value - thunk->address : value;
        v &= (1L << thunk->width) - 1;
        v <<= thunk->left_shift;
        work_area[thunk->address] |= v;
        thunks_done++;
        remove_thunk(ttab, current_thunk);
    }
    
    return thunks_done;
}

typedef struct {
    char buf[512];
    int is_label_def, has_comma, is_end_of_list, eof, error;
    
    FILE *file;
    
    uint64_t pc;
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