#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_LABEL_LEN 10

typedef struct {
    char label[MAX_LABEL_LEN + 1];
    uint64_t value;
} label_def_t;

typedef struct {
    label_def_t *labels;
    int num_labels, max_labels;
} label_tab_t;

int get_label(label_tab_t *ltab, char *label) {
    for (int i = 0; i < ltab->num_labels; i++) {
        if (!strcmp(label, ltab->labels[i].label)) return i;
    }
    
    return -1;
}

int insert_label_def(label_tab_t *ltab, char *label, uint64_t value) {
    if (get_label(ltab, label) != -1) return -1;
    
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
        printf("\n\tThunked [%lu] = %011lo",
            thunk->address, work_area[thunk->address]);
        remove_thunk(ttab, current_thunk);
    }
    return thunks_done;
}

typedef struct {
    char buf[512];
    int is_label_def, has_comma, is_end_of_list, eof, error;
    
    FILE *file;
    
    uint64_t pc;
    
    label_tab_t *ltab;
    thunk_tab_t *ttab;
    uint64_t *work_area;
} assembler_ctx_t;

int assembler_get_label(assembler_ctx_t *ctx, char *label) {
    return get_label(ctx->ltab, label);
}

assembler_ctx_t *new_assembler(
    char *fname, int max_labels, int max_thunks, uint64_t *work_area
) {
    FILE *file = fopen(fname, "r");
    if (file == NULL) return NULL;
    
    assembler_ctx_t *result = calloc(1, sizeof(assembler_ctx_t));
    result->file = file;
    
    result->ltab = new_label_tab(max_labels);
    result->ttab = new_thunk_tab(max_thunks);
    result->work_area = work_area;
    
    return result;
}

void delete_assembler(assembler_ctx_t *ctx) {
    fclose(ctx->file);
    
    delete_label_tab(ctx->ltab);
    delete_thunk_tab(ctx->ttab);
    
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

enum event_type {
    ERROR,
    SYMBOL,
    LABEL_DEF,
    LIST_ITEM,
    LIST_END,
    FILE_END
};

enum event_type get_symbol_type(assembler_ctx_t *ctx) {
    enum event_type result =  ctx->error          ? ERROR
                            : ctx->eof            ? FILE_END
                            : ctx->is_label_def   ? LABEL_DEF
                            : ctx->has_comma      ? LIST_ITEM
                            : ctx->is_end_of_list ? LIST_END
                            :                       SYMBOL
    ;
    
    return result;
}

int assembler_register_label(
    assembler_ctx_t *ctx,
    char *label,
    uint64_t value
) {
    return register_label_do_thunks(
        ctx->ltab, label, value,
        ctx->ttab, ctx->work_area
    );
}

int assembler_insert_thunk(
    assembler_ctx_t *ctx,
    char *label,
    uint64_t address,
    int is_relative,
    int width,
    int left_shift
) {
    return insert_thunk(
        ctx->ttab,
        label,
        address,
        is_relative,
        width,
        left_shift
    );
}

int assembler_get_or_thunk(
    assembler_ctx_t *ctx,
    char *label,
    uint64_t address,
    int is_relative,
    int width,
    int left_shift,
    uint64_t *result
) {
    int label_index = assembler_get_label(
        ctx, label
    );
    
    if (label_index == -1) {
        int thunk = assembler_insert_thunk(
            ctx,
            label,
            address,
            is_relative,
            width,
            left_shift
        );
        if (thunk == -1) return -1;
        else return 0;
    }
    
    uint64_t value = ctx->ltab->labels[label_index].value;
    if (is_relative) value -= address;
    
    *result = value & ((1L << width) - 1);
    *result <<= left_shift;
    
    return 1;
}

#define ADDR_INDIRECT       (1L << 22)

#define ADDR_IMMEDIATE      (0)
#define ADDR_DIRECT_PAGE    (1L << 18)
#define ADDR_PC_RELATIVE    (2L << 18)
#define ADDR_POST_INCREMENT (14L << 18)
#define ADDR_PRE_DECREMENT  (15L << 18)

#define RDC_NUM_GENERAL 16

char *r_general[] = {
    "ac", 
    "xy", 
    "mq", 
    "x0", 
    "x1", 
    "x2", 
    "x3", 
    "x4", 
    "x5", 
    "x6", 
    "x7", 
    "ap", 
    "lr", 
    "sp", 
    "r14",
    "r15"
};

#define RDC_NUM_CONTROL 8

char *r_control[] = {
    "psw0",
    "psw1",
    "fpc",
    "plt",
    "slt",
    "sdr",
    "sflt",
    "cr8"
};

int64_t get_reg(char *rtab[], int max, char *label, char **endptr) {
    if (isdigit(*label)) {
        int64_t result = (int64_t) strtoull(label, endptr, 10);
        if (*endptr == label || result == -1 || result >= max) return -1;
        else return result;
    } else {
        for (int i = 0; i < max; i++) {
            char *p = strstr(label, rtab[i]);
            if (p == label) {
                if (endptr != NULL) *endptr = label + strlen(rtab[i]);
                return i;
            }
        }
        if (endptr != NULL) *endptr = label;
        return -1;
    }
}

int parse_address_field(assembler_ctx_t *ctx, char *field, uint64_t *out) {
    int thunked_label = 0;
    int allow_parens = 0;
    int need_relative = 0;
    
    if (*field == '@') {
        field++;
        *out |= ADDR_INDIRECT;
    }
    
    switch (*field) {
        case '_': {*out |= ADDR_DIRECT_PAGE; field++;} break;
        case '.': {
            *out |= ADDR_PC_RELATIVE;
            need_relative = 1;
            field++;
        } break;
        case '+': {*out |= ADDR_POST_INCREMENT; field++;} break;
        case '=': {*out |= ADDR_PRE_DECREMENT; field++;} break;
        default: allow_parens = 1;
    }
    
    uint64_t displacement;

    if (isdigit(*field) || *field == '-') {
        displacement = (uint64_t) strtoll(field, &field, 10);
        if (displacement <= 0777777) *out |= displacement;
        else return -1;
    } else if (*field == '#') {
        displacement = (uint64_t) strtoull(field + 1, &field, 16);
        if (displacement <= 0777777) *out |= displacement;
        else return -1;
    } else {
        char label[MAX_LABEL_LEN + 1];
        for (int i = 0; i < MAX_LABEL_LEN; i++) {
            if (!(*field) || *field == '(') {
                label[i] = 0;
                break;
            }
            label[i] = *field++;
        }
        label[MAX_LABEL_LEN] = 0;
        
        int status = assembler_get_or_thunk(
            ctx, label, ctx->pc,
            need_relative, 18, 0,
            &displacement
        );
        
        if (status == -1) return -1;
        
        thunked_label = !status;
        if (status) {
            *out |= displacement;
        }
    }
    
    if (*field == '(') {
        if (!allow_parens) return -1;
        
        int64_t reg = get_reg(r_general, RDC_NUM_GENERAL, field + 1, &field);
        if (reg < 3 || reg > 13) return -1;
        if (*field != ')') return -1;
        *out |= reg << 18;
    }
    
    return thunked_label;
}

int main(int argc, char *argv[]) {
    uint64_t work_area[8192];
    assembler_ctx_t *assembler = new_assembler(argv[1], 128, 128, work_area);
    
    while (!read_symbol(assembler)) {
        printf("%-16s is ", assembler->buf);
        switch (get_symbol_type(assembler)) {
            case ERROR:     printf("ERROR"); break;
            case FILE_END:  printf("FILE_END"); break;
            
            case LABEL_DEF: {
                printf("LABEL_DEF@%lu", assembler->pc);
                
                assembler->buf[strlen(assembler->buf) - 1] = 0;
                assembler_register_label(
                    assembler,
                    assembler->buf,
                    assembler->pc
                );
            } break;
            
            case LIST_ITEM: printf("LIST_ITEM"); break;
            case LIST_END:  printf("LIST_END"); break;
            
            case SYMBOL: {
                if (!strcmp(assembler->buf, "ref")) {
                    printf("LABEL_REF@");
                    
                    read_symbol(assembler);
                    switch(get_symbol_type(assembler)) {
                        case SYMBOL: {
                            uint64_t value = 0;
                            int status = parse_address_field(
                                assembler, assembler->buf, &value
                            );
                            
                            if (status == -1) {
                                printf("ERROR");
                            } else {
                                printf(
                                    "%011lo %s", value,
                                    status ? "(thunk)" : ""
                                );
                                assembler->work_area[assembler->pc] = value;
                            }
                        } break;
                        
                        default: printf("ERROR");
                    }
                } else {
                    printf("SYMBOL");
                }
                assembler->pc++;
            } break;
        }
        
        printf("\n");
    }
    
    printf(
        "Ended on %s\n", 
          assembler->eof   ? "EOF"
        : assembler->error ? "error"
        :                    "impossible"
    );
    
    if (assembler->ttab->num_thunks == 0) {
        printf("All references resolved\n");
    } else {
        printf("%d unresolved references\n", assembler->ttab->num_thunks);
    }
    
    delete_assembler(assembler);
    return 0;
}