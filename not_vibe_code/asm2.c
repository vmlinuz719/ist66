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
        printf("\n\tThunked [%09lo] = %012lo",
            thunk->address, work_area[thunk->address]);
        remove_thunk(ttab, current_thunk);
    }
    return thunks_done;
}

typedef struct {
    char buf[513];
    int next, is_label_def, has_comma, is_end_of_list, eof, error;
    
    FILE *file;
    int line_no;
    
    uint64_t asm_offset, pc;
    
    label_tab_t *ltab;
    thunk_tab_t *ttab;
    uint64_t *work_area;
} assembler_ctx_t;

int assembler_get_label(assembler_ctx_t *ctx, char *label) {
    return get_label(ctx->ltab, label);
}

uint64_t assembler_next(assembler_ctx_t *ctx) {
    ctx->pc++;
    return ctx->asm_offset++;
}

uint64_t assembler_set(assembler_ctx_t *ctx, uint64_t new_pc) {
    // TODO: somehow indicate new offset for final output
    ctx->pc = new_pc;
    return ctx->asm_offset++;
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
    while ((isspace(ctx->next) || ctx->next == 0) && ctx->next != EOF) {
        if (ctx->next == '\n') ctx->line_no++;
        ctx->next = fgetc(ctx->file);
    }
    
    if (ctx->next == EOF) {
        if (feof(ctx->file)) {
            ctx->eof = 1;
        }
        if (ferror(ctx->file)) {
            ctx->error = 1;
        }
        return -1;
    }
    
    ctx->is_end_of_list = ctx->has_comma;
    ctx->is_label_def = ctx->has_comma = 0;
    
    int i;
    for (i = 0; i < 512; i++) {
        ctx->buf[i] = ctx->next;
        
        ctx->next = fgetc(ctx->file);
        
        if (isspace(ctx->next)) {
            if (ctx->next == '\n') ctx->line_no++;
            break;
        }
        
        if (ctx->next == EOF) {
            if (ferror(ctx->file)) {
                ctx->error = 1;
                return -1;
            }
            break;
        }
        
        if (ctx->next == ':') {
            if (ctx->is_end_of_list) {
                ctx->error = 1;
                return -1;
            }
            ctx->is_label_def = 1;
            break;
        }
        
        if (ctx->next == ',') {
            ctx->has_comma = 1;
            break;
        }
    }
    ctx->buf[i + 1] = 0;
    
    ctx->next = fgetc(ctx->file);
    while (isspace(ctx->next) && ctx->next != EOF) {
        if (ctx->next == '\n') ctx->line_no++;
        ctx->next = fgetc(ctx->file);
    }
    
    if (ctx->next == EOF) {
        if (ferror(ctx->file)) {
            ctx->error = 1;
            return -1;
        }
        return 0;
    }
    
    if (ctx->next == ':') {
        if (ctx->is_label_def || ctx->has_comma || ctx->is_end_of_list) {
            ctx->error = 1;
            return -1;
        }
        ctx->is_label_def = 1;
        ctx->next = fgetc(ctx->file);
    }
    
    if (ctx->next == ',') {
        if (ctx->is_label_def || ctx->has_comma) {
            ctx->error = 1;
            return -1;
        }
        
        ctx->has_comma = 1;
        ctx->next = fgetc(ctx->file);
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
    "mq", 
    "xy", 
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

#define RDC_NUM_FLOAT 4

char *r_float[] = {
    "f0",
    "f1",
    "f2",
    "f3"
};

int64_t get_num(int max, char *label, char **endptr, int base) {
    int64_t result = (int64_t) strtoull(label, endptr, base);
    if ((endptr != NULL && *endptr == label) || result == -1 || result >= max) return -1;
    else return result;
}

int64_t get_reg(char *rtab[], int max, char *label, char **endptr) {
    if (isdigit(*label)) {
        return get_num(max, label, endptr, 10);
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

    if (*field == '0') {
        displacement = (uint64_t) strtoull(field + 1, &field, 8);
        if (displacement <= 0777777) *out |= displacement;
        else return -1;
    } else if (isdigit(*field) || *field == '-') {
        displacement = (uint64_t) strtoll(field, &field, 10);
        *out |= displacement & 0777777;
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
            ctx, label, ctx->asm_offset,
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

typedef struct {
    char *mnemonic;
    uint64_t base;
    int (*assemble)(assembler_ctx_t *, uint64_t);
} assembler_entry_t;

int assemble_unary(assembler_ctx_t *ctx, uint64_t opcode) {
    ctx->work_area[assembler_next(ctx)] = opcode;
    return 0;
}

int assemble_mr(assembler_ctx_t *ctx, uint64_t opcode) {
    read_symbol(ctx);
    switch(get_symbol_type(ctx)) {
        case SYMBOL: {
            uint64_t value = 0;
            int status = parse_address_field(
                ctx, ctx->buf, &value
            );

            if (status == -1) {
                return -1;
            } else {
                ctx->work_area[ctx->asm_offset] = value | opcode;
            }
        } break;

        default: {
            return -1;
        }
    }
    assembler_next(ctx);
    return 0;
}

int assemble_am(assembler_ctx_t *ctx, uint64_t opcode) {
    read_symbol(ctx);
    uint64_t value = 0;
    switch(get_symbol_type(ctx)) {
        case LIST_ITEM: {
            int64_t reg = get_reg(r_general, RDC_NUM_GENERAL, ctx->buf, NULL);
            if (reg == -1) {
                return -1;
            }

            value |= reg << 23;
            read_symbol(ctx);
            if (get_symbol_type(ctx) != LIST_END) {
                return -1;
            }
        }
        case SYMBOL: {
            int status = parse_address_field(
                ctx, ctx->buf, &value
            );

            if (status == -1) {
                return -1;
            } else {
                ctx->work_area[ctx->asm_offset] = value | opcode;
            }
        } break;

        default: {
            return -1;
        }
    }
    assembler_next(ctx);
    return 0;
}

char *tests[] = {
    "no",
    "sk",
    "cn",
    "cz",
    "rn",
    "rz",
    "bn",
    "bz"
};

int assemble_aa_r(assembler_ctx_t *ctx, uint64_t opcode) {
    int is_m_type = (tolower(ctx->buf[3]) == 'm');
    uint64_t value = ((opcode >> 4) << 32) | ((opcode & 7) << 20);

    int index = 4;
    if (tolower(ctx->buf[index]) == 't') {
        index++;
        value |= 1L << 13;
    }
    if (tolower(ctx->buf[index]) == 'r') {
        index++;
        value |= 1L << 12;
    }
    switch (tolower(ctx->buf[index])) {
        case 'z': {index++; value |= 1L << 18;} break;
        case 's': {index++; value |= 2L << 18;} break;
        case 'c': {index++; value |= 3L << 18;} break;
    }
    if (tolower(ctx->buf[index]) == 'n') {
        index++;
        value |= 1L << 31;
    }
    if (ctx->buf[index] == '.') {
        index++;
        char *test_name = &(ctx->buf[index]);
        uint64_t test = 0;
        for (int i = 0; i < 8; i++) {
            if (!strncmp(test_name, tests[i], 2)) {
                test = i << 15;
                break;
            }
        }
        if (!test) return -1;
        value |= test;
        index += 2;
    }
    if (ctx->buf[index] != 0) return -1;

    read_symbol(ctx);
    if (get_symbol_type(ctx) != LIST_ITEM) return -1;
    int64_t src = get_reg(r_general, RDC_NUM_GENERAL, ctx->buf, NULL);
    if (src == -1) return -1;
    value |= src << 27;

    read_symbol(ctx);
    enum event_type evt = get_symbol_type(ctx);
    if (evt != LIST_ITEM && evt != LIST_END) return -1;
    int64_t tgt = get_reg(r_general, RDC_NUM_GENERAL, ctx->buf, NULL);
    if (tgt == -1) return -1;
    value |= tgt << 23;
    if (evt == LIST_END) {
        ctx->work_area[assembler_next(ctx)] = value;
        return 0;
    }

    read_symbol(ctx);
    evt = get_symbol_type(ctx);
    if (evt != LIST_ITEM && evt != LIST_END) return -1;
    int64_t arg2 = get_num(37, ctx->buf, NULL, 10);
    if (arg2 == -1) return -1;
    value |= arg2 << (is_m_type ? 6 : 0);
    if (evt == LIST_END) {
        ctx->work_area[assembler_next(ctx)] = value;
        return 0;
    }

    read_symbol(ctx);
    if (get_symbol_type(ctx) != LIST_END) return -1;
    int64_t arg3 = get_num(37, ctx->buf, NULL, 10);
    if (arg3 == -1) return -1;
    value |= arg3 << (is_m_type ? 0 : 6);
    ctx->work_area[assembler_next(ctx)] = value;
    return 0;
}

int assemble_aa_s(assembler_ctx_t *ctx, uint64_t opcode) {
    uint64_t value = ((opcode >> 4) << 32) | ((opcode & 7) << 20);
    value |= 1L << 14;

    int index = 4;

    if (tolower(ctx->buf[index]) == 'r') {
        index++;
        value |= 1L << 12;
    }
    switch (tolower(ctx->buf[index])) {
        case 'z': {index++; value |= 1L << 18;} break;
        case 's': {index++; value |= 2L << 18;} break;
        case 'c': {index++; value |= 3L << 18;} break;
    }
    if (tolower(ctx->buf[index]) == 'n') {
        index++;
        value |= 1L << 31;
    }
    if (ctx->buf[index] == '.') {
        index++;
        char *test_name = &(ctx->buf[index]);
        uint64_t test = 0;
        for (int i = 0; i < 8; i++) {
            if (!strncmp(test_name, tests[i], 2)) {
                test = i << 15;
                break;
            }
        }
        if (!test) return -1;
        value |= test;
        index += 2;
    }
    if (ctx->buf[index] != 0) return -1;

    read_symbol(ctx);
    if (get_symbol_type(ctx) != LIST_ITEM) return -1;
    int64_t src = get_reg(r_general, RDC_NUM_GENERAL, ctx->buf, NULL);
    if (src == -1) return -1;
    value |= src << 27;

    read_symbol(ctx);
    if (get_symbol_type(ctx) != LIST_ITEM) return -1;
    int64_t tgt = get_reg(r_general, RDC_NUM_GENERAL, ctx->buf, NULL);
    if (tgt == -1) return -1;
    value |= tgt << 23;

    read_symbol(ctx);
    enum event_type evt = get_symbol_type(ctx);
    if (evt != LIST_ITEM && evt != LIST_END) return -1;
    int64_t dst = get_reg(r_general, RDC_NUM_GENERAL, ctx->buf, NULL);
    if (dst == -1) return -1;
    value |= dst << 6;
    if (evt == LIST_END) {
        ctx->work_area[assembler_next(ctx)] = value;
        return 0;
    }

    read_symbol(ctx);
    if (get_symbol_type(ctx) != LIST_END) return -1;
    int64_t arg3 = get_num(37, ctx->buf, NULL, 10);
    if (arg3 == -1) return -1;
    value |= arg3 << 0;
    ctx->work_area[assembler_next(ctx)] = value;
    return 0;
}

int assemble_aa_i(assembler_ctx_t *ctx, uint64_t opcode) {
    uint64_t value = ((opcode >> 4) << 32) | ((opcode & 7) << 20);
    value |= 3L << 13;

    int index = 4;

    switch (tolower(ctx->buf[index])) {
        case 'z': {index++; value |= 1L << 18;} break;
        case 's': {index++; value |= 2L << 18;} break;
        case 'c': {index++; value |= 3L << 18;} break;
    }
    if (tolower(ctx->buf[index]) == 'n') {
        index++;
        value |= 1L << 31;
    }
    if (ctx->buf[index] == '.') {
        index++;
        char *test_name = &(ctx->buf[index]);
        uint64_t test = 0;
        for (int i = 0; i < 8; i++) {
            if (!strncmp(test_name, tests[i], 2)) {
                test = i << 15;
                break;
            }
        }
        if (!test) return -1;
        value |= test;
        index += 2;
    }
    if (ctx->buf[index] != 0) return -1;

    read_symbol(ctx);
    if (get_symbol_type(ctx) != LIST_ITEM) return -1;
    int64_t src = get_reg(r_general, RDC_NUM_GENERAL, ctx->buf, NULL);
    if (src == -1) return -1;
    value |= src << 27;

    read_symbol(ctx);
    if (get_symbol_type(ctx) != LIST_ITEM) return -1;
    int64_t tgt = get_reg(r_general, RDC_NUM_GENERAL, ctx->buf, NULL);
    if (tgt == -1) return -1;
    value |= tgt << 23;

    read_symbol(ctx);
    if (get_symbol_type(ctx) != LIST_END) return -1;
    char *field = ctx->buf;
    uint64_t arg2;
    if (*field == '0') {
        arg2 = (uint64_t) strtoull(field + 1, &field, 8);
        if (arg2 > 017777) return -1;
    } else if (isdigit(*field) || *field == '-') {
        arg2 = (uint64_t) strtoll(field, &field, 10);
    } else if (*field == '#') {
        arg2 = (uint64_t) strtoull(field + 1, &field, 16);
        if (arg2 > 017777) return -1;
    } else return -1;
    value |= arg2 & 017777;
    ctx->work_area[assembler_next(ctx)] = value;
    return 0;
}

int assemble_aa_var(assembler_ctx_t *ctx, uint64_t opcode) {
    switch (tolower(ctx->buf[3])) {
        case 'r':
        case 'm': return assemble_aa_r(ctx, opcode);
        case 's': return assemble_aa_s(ctx, opcode);
        case 'i': return assemble_aa_i(ctx, opcode);
        default:  return -1;
    }
}

assembler_entry_t var_instructions[] = {
    {"com",     0xE0,           assemble_aa_var},
    {"ngt",     0xE1,           assemble_aa_var},
    {"mov",     0xE2,           assemble_aa_var},
    {"inc",     0xE3,           assemble_aa_var},
    {"adc",     0xE4,           assemble_aa_var},
    {"sub",     0xE5,           assemble_aa_var},
    {"add",     0xE6,           assemble_aa_var},
    {"and",     0xE7,           assemble_aa_var},
    {"bis",     0xF2,           assemble_aa_var},
    {"xor",     0xF6,           assemble_aa_var},
    {"pct",     0xF7,           assemble_aa_var},
};

assembler_entry_t instructions[] = {
    {"nop",     0000002000001,  assemble_unary},
    {"retl",    0000014000000,  assemble_unary},
    {"jmp",     0000000000000,  assemble_mr},
    {"callr",   0000040000000,  assemble_mr},
    {"inctnz",  0000100000000,  assemble_mr},
    {"dectnz",  0000140000000,  assemble_mr},
    {"tstmnz",  0000200000000,  assemble_mr},
    {"tstmz",   0000240000000,  assemble_mr},
    {"calls",   0000700000000,  assemble_mr},
    {"rets",    0000740000000,  assemble_unary},
    {"retsd",   0000740000000,  assemble_mr},
    
    {"mul",     0001000000000,  assemble_mr},
    {"fmadd",   0001040000000,  assemble_mr},
    {"fmsub",   0001100000000,  assemble_mr},
    {"div",     0001140000000,  assemble_mr},
    {"umul",    0001200000000,  assemble_mr},
    {"ufmadd",  0001240000000,  assemble_mr},
    {"ufmsub",  0001300000000,  assemble_mr},
    {"udiv",    0001340000000,  assemble_mr},
    
    {"reti",    0010000000000,  assemble_unary},
    {"retid",   0010000000000,  assemble_mr},
    {"retlmi",  0010040000000,  assemble_mr},
    {"ldmask",  0010100000000,  assemble_mr},
    {"lmwait",  0010140000000,  assemble_mr},
    {"stmask",  0010200000000,  assemble_mr},
    {"invlsg",  0010240000000,  assemble_mr},
    {"invlpg",  0010300000000,  assemble_mr},
    {"retsv",   0010340000000,  assemble_unary},
    {"retsvd",  0010340000000,  assemble_mr},
    
    {"edit",    0041000000000,  assemble_am},
    {"edits",   0042000000000,  assemble_am},
    {"ldea",    0043000000000,  assemble_am},
    {"addea",   0044000000000,  assemble_am},
    {"inctne",  0045000000000,  assemble_am},
    {"dectne",  0046000000000,  assemble_am},
    {"ldeas",   0047000000000,  assemble_am},
    {"ldcom",   0050000000000,  assemble_am},
    {"ldneg",   0051000000000,  assemble_am},
    {"ld",      0052000000000,  assemble_am},
    {"st",      0053000000000,  assemble_am},
    {"addcom",  0054000000000,  assemble_am},
    {"sub",     0055000000000,  assemble_am},
    {"add",     0056000000000,  assemble_am},
    {"and",     0057000000000,  assemble_am},
    {"or",      0062000000000,  assemble_am},
    {"xor",     0066000000000,  assemble_am},
    
    {"hlt",     0070002000001,  assemble_unary},
    {"wait",    0070000000000,  assemble_am},
    {"intr",    0071000000000,  assemble_am},
    {"ldkey",   0072000000000,  assemble_am},
    {"stkey",   0073000000000,  assemble_am},
    {"ldctl",   0074000000000,  assemble_am},
    {"stctl",   0075000000000,  assemble_am},
    {"ldtrt",   0076000000000,  assemble_am},
};

int main(int argc, char *argv[]) {
    uint64_t work_area[8192];
    assembler_ctx_t *assembler = new_assembler(argv[1], 128, 128, work_area);
    
    while (!assembler->error && !read_symbol(assembler)) {
        printf("%-16s is ", assembler->buf);
        switch (get_symbol_type(assembler)) {
            case ERROR:     printf("ERROR"); break;
            case FILE_END:  printf("FILE_END"); break;
            
            case LABEL_DEF: {
                printf("LABEL_DEF@%lo", assembler->asm_offset);
                
                assembler_register_label(
                    assembler,
                    assembler->buf,
                    assembler->asm_offset
                );
            } break;
            
            case LIST_ITEM: printf("LIST_ITEM"); break;
            case LIST_END:  printf("LIST_END"); break;
            
            case SYMBOL: {
                int assembled = 0;
                uint64_t current_pc = assembler->asm_offset;
                for (
                    int i = 0;
                    i < sizeof(instructions) / sizeof(assembler_entry_t);
                    i++
                ) {
                    if (!strcmp(assembler->buf, instructions[i].mnemonic)) {
                        if (
                            instructions[i].assemble(
                                assembler, instructions[i].base
                            ) == -1
                        ) assembler->error = 1;
                        else assembled = 1;
                        break;
                    }
                }
                if (!assembled) {
                    for (
                        int i = 0;
                        i < sizeof(var_instructions) / sizeof(assembler_entry_t);
                        i++
                    ) {
                        if (
                            !strncmp(
                                assembler->buf,
                                var_instructions[i].mnemonic,
                                3
                            )
                        ) {
                            if (
                                var_instructions[i].assemble(
                                    assembler, var_instructions[i].base
                                ) == -1
                            ) assembler->error = 1;
                            else assembled = 1;
                            break;
                        }
                    }
                }
                if (!assembled) {
                    printf("ERROR");
                    assembler->error = 1;
                } else {
                    printf("%012lo", assembler->work_area[current_pc]);
                }
            } break;
        }
        
        printf("\n");
    }
    
    if (assembler->error) {
        printf("Error on line %d\n", assembler->line_no);
    } else if (assembler->ttab->num_thunks == 0) {
        printf("All references resolved\n");
    } else {
        printf("%d unresolved references\n", assembler->ttab->num_thunks);
    }
    
    delete_assembler(assembler);
    return 0;
}
