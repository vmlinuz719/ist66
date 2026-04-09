/*
 * Begin AI-generated code
 */

#include <stdint.h>
#include <stdio.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#include "cpu.h"
#include "alu.h"
#include "panel.h"

#define FONT_PATH "/usr/share/fonts/truetype/noto/NotoSansMono-Bold.ttf"
#define FONT_SIZE 16
#define FONT_SIZE_SMALL 12

/* --- Seven-segment display geometry --- */

#define SEG_W       20   /* digit cell width */
#define SEG_H       38   /* digit cell height */
#define SEG_T        3   /* segment thickness */
#define SEG_GAP      6   /* gap between digits */
#define SEG_PAD      2   /* inset from cell edge */

/* Segment bitmasks (standard A-G mapping) */
#define SA 0x01  /* top */
#define SB 0x02  /* top-right */
#define SC 0x04  /* bottom-right */
#define SD 0x08  /* bottom */
#define SE 0x10  /* bottom-left */
#define SF 0x20  /* top-left */
#define SG 0x40  /* middle */

static const uint8_t seg_table[8] = {
    SA|SB|SC|SD|SE|SF,      /* 0 */
    SB|SC,                   /* 1 */
    SA|SB|SD|SE|SG,          /* 2 */
    SA|SB|SC|SD|SG,          /* 3 */
    SB|SC|SF|SG,             /* 4 */
    SA|SC|SD|SF|SG,          /* 5 */
    SA|SC|SD|SE|SF|SG,       /* 6 */
    SA|SB|SC,                /* 7 */
};

/* --- Layout constants --- */

#define LABEL_W         60    /* width reserved for row labels */
#define DISPLAY_X       (20 + LABEL_W)  /* left edge of digit displays */
#define LABEL_X         10    /* left edge of labels */
#define ADDR_Y          20    /* top of address row */
#define DATA_Y          80    /* top of data row */
#define ADDR_DIGITS      9
#define DATA_DIGITS     12

#define BTN_W           50
#define BTN_H           40
#define BTN_GAP          8
#define BTN_Y          140
#define BTN_X           36

#define NUM_BUTTONS     22    /* 0-7, Addr, Clr, Load, Ld+, Str, St+, LdAC, StAC, LdCt, StCt, Run, Halt, Step, StPC */
#define NUM_INDICATORS  3

#define BTN_COLS        4
#define BTN_ROWS        4     /* row 0: 0-3, row 1: 4-7, row 2: Addr/Clr, row 3: mem ops */

/* Right-side columns for register and CPU control buttons */
#define BTN_RX          (BTN_X + BTN_COLS * (BTN_W + BTN_GAP) + BTN_GAP)
#define BTN_RX2         (BTN_RX + BTN_W + BTN_GAP)

#define SCREEN_WIDTH_DISPLAYS (DISPLAY_X + DATA_DIGITS * (SEG_W + SEG_GAP) + 40)
#define SCREEN_WIDTH_BUTTONS  (BTN_RX2 + (2 * BTN_W) + 2 * BTN_GAP + 20)
#define SCREEN_WIDTH   ((SCREEN_WIDTH_DISPLAYS > SCREEN_WIDTH_BUTTONS) ? SCREEN_WIDTH_DISPLAYS : SCREEN_WIDTH_BUTTONS)
#define SCREEN_HEIGHT  (BTN_Y + BTN_ROWS * (BTN_H + BTN_GAP) + 12)

/* Button label strings */
static const char *btn_labels[NUM_BUTTONS] = {
    "0", "1", "2", "3", "4", "5", "6", "7",
    "Addr", "Clr", "Load", "Ld +", "Str", "St +",
    "LdAC", "StAC", "LdCt", "StCt",
    "Run", "Halt", "Step", "StPC"
};

typedef struct {
    SDL_Rect box;
    const char *label;
    TTF_Font *font;
    void (*action_callback)(void *, int);
    void *instance_data;

    int color[3];
    int pressed_color[3];
    int text_color[3];
    int border_color[3];
    int id, hotkey, pressed;
} button_t;

button_t *make_button(
    const char *label,
    char *font_name,
    int font_size,
    int id,
    int x, int y, int width, int height,
    int color[3],
    int pressed_color[3],
    int text_color[3],
    int border_color[3],
    int hotkey,
    void (*action_callback)(void *, int),
    void *instance_data
) {
    TTF_Font *font = TTF_OpenFont(font_name, font_size);
    if (!font) {
        fprintf(stderr, "Panel: font load failed: %s\n", TTF_GetError());
        return NULL;
    }

    button_t *button = malloc(sizeof(button_t));
    button->id = id;
    button->label = label;
    button->font = font;
    button->box = (SDL_Rect) {x, y, width, height};

    memcpy(button->color, color, 3 * sizeof(int));
    memcpy(button->pressed_color, pressed_color, 3 * sizeof(int));
    memcpy(button->text_color, text_color, 3 * sizeof(int));
    memcpy(button->border_color, border_color, 3 * sizeof(int));

    button->action_callback = action_callback;
    button->instance_data = instance_data;

    button->hotkey = hotkey;
    button->pressed = 0;

    return button;
}

button_t *make_button_default(
    const char *label,
    int id,
    int x, int y, int width, int height,
    int hotkey,
    void (*action_callback)(void *, int),
    void *instance_data
) {
    int default_color[3] = {50, 50, 50};
    int default_pressed[3] = {80, 80, 80};
    int default_foreground[3] = {140, 140, 140};

    return make_button(
        label,
        FONT_PATH,
        FONT_SIZE,
        id,
        x,
        y,
        width,
        height,
        default_color,
        default_pressed,
        default_foreground,
        default_foreground,
        hotkey,
        action_callback,
        instance_data
    );
}

button_t *make_indicator_green(
    const char *label,
    int id,
    int x, int y,
    void *instance_data
) {
    int default_color[3] = {0, 50, 0};
    int default_pressed[3] = {0, 255, 0};
    int default_foreground[3] = {140, 140, 140};

    return make_button(
        label,
        FONT_PATH,
        FONT_SIZE_SMALL,
        id,
        x,
        y,
        BTN_W,
        BTN_H,
        default_color,
        default_pressed,
        default_foreground,
        default_foreground,
        -1,
        NULL,
        instance_data
    );
}

button_t *make_indicator_amber(
    const char *label,
    int id,
    int x, int y,
    void *instance_data
) {
    int default_color[3] = {50, 33, 0};
    int default_pressed[3] = {255, 170, 0};
    int default_foreground[3] = {140, 140, 140};

    return make_button(
        label,
        FONT_PATH,
        FONT_SIZE_SMALL,
        id,
        x,
        y,
        BTN_W,
        BTN_H,
        default_color,
        default_pressed,
        default_foreground,
        default_foreground,
        -1,
        NULL,
        instance_data
    );
}

button_t *make_indicator_red(
    const char *label,
    int id,
    int x, int y,
    void *instance_data
) {
    int default_color[3] = {50, 0, 0};
    int default_pressed[3] = {255, 0, 0};
    int default_foreground[3] = {140, 140, 140};

    return make_button(
        label,
        FONT_PATH,
        FONT_SIZE_SMALL,
        id,
        x,
        y,
        BTN_W,
        BTN_H,
        default_color,
        default_pressed,
        default_foreground,
        default_foreground,
        -1,
        NULL,
        instance_data
    );
}

void destroy_button(button_t *button) {
    TTF_CloseFont(button->font);
    free(button);
}



/* --- Panel state --- */

typedef struct {
    int running;
    pthread_t thread;
    int updated;
    pthread_mutex_t update_lock;
    ist66_cu_t *cpu;

    SDL_Window *window;
    SDL_Renderer *render;

    uint32_t addr_reg;   /* 27-bit address */
    uint64_t data_reg;   /* 36-bit data */

    int locked;

    button_t *new_buttons[NUM_BUTTONS];
    button_t *indicators[NUM_INDICATORS];
    TTF_Font *font;
} panel_ctx_t;

/* --- Drawing helpers --- */

static void draw_text_centered(SDL_Renderer *r, TTF_Font *font,
                                const char *text, SDL_Rect *rect,
                                int cr, int cg, int cb) {
    SDL_Color color = {cr, cg, cb, 255};
    SDL_Surface *surf = TTF_RenderText_Blended(font, text, color);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    if (!tex) { SDL_FreeSurface(surf); return; }
    SDL_Rect dst = {
        rect->x + (rect->w - surf->w) / 2,
        rect->y + (rect->h - surf->h) / 2,
        surf->w, surf->h
    };
    SDL_RenderCopy(r, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

static void draw_button(SDL_Renderer *r, button_t *button) {
    if (button->pressed) {
        SDL_SetRenderDrawColor(
            r,
            button->pressed_color[0],
            button->pressed_color[1],
            button->pressed_color[2],
            255
        );
    } else {
        SDL_SetRenderDrawColor(
            r,
            button->color[0],
            button->color[1],
            button->color[2],
            255
        );
    }
    SDL_RenderFillRect(r, &button->box);

    SDL_SetRenderDrawColor(
        r,
        button->border_color[0],
        button->border_color[1],
        button->border_color[2],
        255
    );
    SDL_RenderDrawRect(r, &button->box);

    draw_text_centered(r, button->font, button->label,
        &button->box,
        button->text_color[0],
        button->text_color[1],
        button->text_color[2]
    );
}

int get_indicator_state(button_t *button) {
    int old_pressed = button->pressed;
    button->pressed = (((*(int *)button->instance_data) >> button->id) & 1);
    return button->pressed != old_pressed;
}

static void draw_indicator(SDL_Renderer *r, button_t *button) {
    int *color;

    if (button->pressed) {
        color = button->pressed_color;
    } else {
        color = button->color;
    }

    filledCircleRGBA(
        r,
        button->box.x + (BTN_W / 2),
        button->box.y + (BTN_H / 4),
        BTN_H / 4,
        color[0], color[1], color[2], 255
    );

    color = button->border_color;
    circleRGBA(
        r,
        button->box.x + (BTN_W / 2),
        button->box.y + (BTN_H / 4),
        BTN_H / 4,
        color[0], color[1], color[2], 255
    );

    SDL_Rect text_box = {
        button->box.x,
        button->box.y + (BTN_H / 2),
        BTN_W, BTN_H / 2
    };

    draw_text_centered(r, button->font, button->label,
                       &text_box,
                       button->text_color[0],
                       button->text_color[1],
                       button->text_color[2]
    );
}

static void draw_segment(SDL_Renderer *r, int x, int y, uint8_t segs,
                          int sr, int sg, int sb) {
    /* dim color for unlit segments */
    int dr = sr / 8, dg = sg / 8, db = sb / 8;
    int hw = SEG_W - 2 * SEG_PAD;        /* horizontal seg length */
    int vh = (SEG_H - 2 * SEG_PAD) / 2;  /* vertical seg length (half) */
    int lx = x + SEG_PAD;                /* left x */
    int rx = x + SEG_W - SEG_PAD - SEG_T; /* right x */
    int ty = y + SEG_PAD;                /* top y */
    int my = y + SEG_H / 2 - SEG_T / 2;  /* middle y */
    int by = y + SEG_H - SEG_PAD - SEG_T; /* bottom y */

    SDL_Rect seg;

    /* A - top horizontal */
    seg = (SDL_Rect){lx, ty, hw, SEG_T};
    if (segs & SA) SDL_SetRenderDrawColor(r, sr, sg, sb, 255);
    else           SDL_SetRenderDrawColor(r, dr, dg, db, 255);
    SDL_RenderFillRect(r, &seg);

    /* D - bottom horizontal */
    seg = (SDL_Rect){lx, by, hw, SEG_T};
    if (segs & SD) SDL_SetRenderDrawColor(r, sr, sg, sb, 255);
    else           SDL_SetRenderDrawColor(r, dr, dg, db, 255);
    SDL_RenderFillRect(r, &seg);

    /* G - middle horizontal */
    seg = (SDL_Rect){lx, my, hw, SEG_T};
    if (segs & SG) SDL_SetRenderDrawColor(r, sr, sg, sb, 255);
    else           SDL_SetRenderDrawColor(r, dr, dg, db, 255);
    SDL_RenderFillRect(r, &seg);

    /* F - top-left vertical */
    seg = (SDL_Rect){lx, ty, SEG_T, vh};
    if (segs & SF) SDL_SetRenderDrawColor(r, sr, sg, sb, 255);
    else           SDL_SetRenderDrawColor(r, dr, dg, db, 255);
    SDL_RenderFillRect(r, &seg);

    /* B - top-right vertical */
    seg = (SDL_Rect){rx, ty, SEG_T, vh};
    if (segs & SB) SDL_SetRenderDrawColor(r, sr, sg, sb, 255);
    else           SDL_SetRenderDrawColor(r, dr, dg, db, 255);
    SDL_RenderFillRect(r, &seg);

    /* E - bottom-left vertical */
    seg = (SDL_Rect){lx, my, SEG_T, vh};
    if (segs & SE) SDL_SetRenderDrawColor(r, sr, sg, sb, 255);
    else           SDL_SetRenderDrawColor(r, dr, dg, db, 255);
    SDL_RenderFillRect(r, &seg);

    /* C - bottom-right vertical */
    seg = (SDL_Rect){rx, my, SEG_T, vh};
    if (segs & SC) SDL_SetRenderDrawColor(r, sr, sg, sb, 255);
    else           SDL_SetRenderDrawColor(r, dr, dg, db, 255);
    SDL_RenderFillRect(r, &seg);
}

static void draw_octal_row(SDL_Renderer *r, int x, int y, int ndigits,
                            uint64_t value, int cr, int cg, int cb) {
    for (int i = 0; i < ndigits; i++) {
        int shift = (ndigits - 1 - i) * 3;
        int digit = (value >> shift) & 7;
        draw_segment(r, x + i * (SEG_W + SEG_GAP), y,
                     seg_table[digit], cr, cg, cb);
    }
}

static int point_in_rect(int px, int py, SDL_Rect *r) {
    return px >= r->x && px < r->x + r->w &&
           py >= r->y && py < r->y + r->h;
}

void do_button_action(void *vpanel, int i) {
    panel_ctx_t *panel = (panel_ctx_t *) vpanel;
    if (i < 8) {
        panel->data_reg = ((panel->data_reg << 3) | i) & MASK_36;
    } else if (i == 8) {
        panel->addr_reg = panel->data_reg & MASK_ADDR;
    } else if (i == 9) {
        panel->data_reg = 0;
    } else if (i == 10) { /* Load */
        uint32_t a = panel->addr_reg & MASK_ADDR;
        if (a < panel->cpu->mem_size)
            panel->data_reg = panel->cpu->memory[a] & MASK_36;
    } else if (i == 11) { /* Load Next */
        panel->addr_reg = (panel->addr_reg + 1) & MASK_ADDR;
        uint32_t a = panel->addr_reg;
        if (a < panel->cpu->mem_size)
            panel->data_reg = panel->cpu->memory[a] & MASK_36;
    } else if (i == 12 && !(panel->locked)) { /* Store */
        uint32_t a = panel->addr_reg & MASK_ADDR;
        if (a < panel->cpu->mem_size)
            panel->cpu->memory[a] = panel->data_reg & MASK_36;
    } else if (i == 13 && !(panel->locked)) { /* Store Next */
        panel->addr_reg = (panel->addr_reg + 1) & MASK_ADDR;
        uint32_t a = panel->addr_reg;
        if (a < panel->cpu->mem_size)
            panel->cpu->memory[a] = panel->data_reg & MASK_36;
    } else if (i == 14) { /* Ld AC */
        int r = panel->addr_reg & 0xF;
        panel->data_reg = panel->cpu->a[r] & MASK_36;
    } else if (i == 15 && !(panel->locked)) { /* St AC */
        int r = panel->addr_reg & 0xF;
        panel->cpu->a[r] = panel->data_reg & MASK_36;
    } else if (i == 16) { /* Ld Ct */
        int r = panel->addr_reg & 0x7;
        panel->data_reg = panel->cpu->c[r] & MASK_36;
    } else if (i == 17 && !(panel->locked)) { /* St Ct */
        int r = panel->addr_reg & 0x7;
        panel->cpu->c[r] = panel->data_reg & MASK_36;
    } else if (i == 18 && !(panel->locked)) { /* Run */
        if (!panel->cpu->running)
            start_cpu(panel->cpu, 0);
    } else if (i == 19 && !(panel->locked)) { /* Halt */
        if (panel->cpu->running)
            stop_cpu(panel->cpu);
    } else if (i == 20 && !(panel->locked)) { /* Step */
        if (panel->cpu->exit)
            start_cpu(panel->cpu, 1);
    } else if (i == 21 && !(panel->locked)) { /* StPC */
        set_pc(panel->cpu, panel->addr_reg & MASK_ADDR);
    }
}

/* --- Main panel thread --- */

int panel_do_init(void *ctx) {
    panel_ctx_t *panel = (panel_ctx_t *) ctx;

    panel->window = SDL_CreateWindow(
        "RDC-700 Programmer's Panel",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN
    );
    if (!panel->window) {
        fprintf(stderr, "Panel: window creation failed: %s\n", SDL_GetError());
        return -1;
    }

    panel->render = SDL_CreateRenderer(
        panel->window, -1,
        SDL_RENDERER_ACCELERATED
    );
    if (!panel->render) {
        SDL_DestroyWindow(panel->window);
        fprintf(stderr, "Panel: renderer creation failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Load font */
    if (TTF_Init() < 0) {
        fprintf(stderr, "Panel: TTF_Init failed: %s\n", TTF_GetError());
        SDL_DestroyRenderer(panel->render);
        SDL_DestroyWindow(panel->window);
        return -1;
    }
    panel->font = TTF_OpenFont(FONT_PATH, FONT_SIZE);
    if (!(panel->font)) {
        fprintf(stderr, "Panel: font load failed: %s\n", TTF_GetError());
        TTF_Quit();
        SDL_DestroyRenderer(panel->render);
        SDL_DestroyWindow(panel->window);
        return -1;
    }

    /* Set up button positions: 4-column grid */
    for (int i = 0; i < 8; i++) {
        int col = i % BTN_COLS;
        int row = i / BTN_COLS;

        panel->new_buttons[i] = make_button_default(
            btn_labels[i],
            i,
            BTN_X + col * (BTN_W + BTN_GAP),
            BTN_Y + row * (BTN_H + BTN_GAP),
            BTN_W, BTN_H,
            SDLK_0 + i,
            do_button_action,
            panel
        );

    }
    /* Addr button — row 2, cols 0-1 */
    panel->new_buttons[8] = make_button_default(
        btn_labels[8],
        8,
        BTN_X,
        BTN_Y + 2 * (BTN_H + BTN_GAP),
        2 * BTN_W + BTN_GAP, BTN_H,
        SDLK_a,
        do_button_action,
        panel
    );

    /* Clr button — row 3, cols 0-1 */
    panel->new_buttons[9] = make_button_default(
        btn_labels[9],
        9,
        BTN_X,
        BTN_Y + 3 * (BTN_H + BTN_GAP),
        2 * BTN_W + BTN_GAP, BTN_H,
        SDLK_c,
        do_button_action,
        panel
    );

    /* Memory operation buttons — block of four next to Addr,Clr */
    int mem_op_key[4] = {
        SDLK_l,
        SDLK_SEMICOLON,
        SDLK_s,
        SDLK_d
    };
    for (int i = 0; i < 4; i++) {
        panel->new_buttons[10 + i] = make_button_default(
            btn_labels[10 + i],
            i,
            BTN_X + (2 + (i % 2)) * (BTN_W + BTN_GAP),
            BTN_Y + (2 + (i / 2)) * (BTN_H + BTN_GAP),
            BTN_W, BTN_H,
            mem_op_key[i],
            do_button_action,
            panel
        );
    }

    /* Register buttons — right-side column, rows 0-3 */
    int reg_op_key[4] = {
        SDLK_COMMA,
        SDLK_z,
        SDLK_PERIOD,
        SDLK_x
    };
    for (int i = 0; i < 4; i++) {
        panel->new_buttons[14 + i] = make_button_default(
            btn_labels[14 + i],
            i,
            BTN_RX,
            BTN_Y + i * (BTN_H + BTN_GAP),
            BTN_W, BTN_H,
            reg_op_key[i],
            do_button_action,
            panel
        );
    }

    /* CPU control buttons — second right-side column, rows 0-3 */
    int ctl_op_key[4] = {
        SDLK_r,
        SDLK_h,
        SDLK_t,
        SDLK_p
    };
    for (int i = 0; i < 4; i++) {
        panel->new_buttons[18 + i] = make_button_default(
            btn_labels[18 + i],
            i,
            BTN_RX2,
            BTN_Y + i * (BTN_H + BTN_GAP),
            BTN_W, BTN_H,
            ctl_op_key[i],
            do_button_action,
            panel
        );
    }

    /* Run indicator */
    panel->indicators[0] = make_indicator_green(
        "CPU RUN",
        0,
        BTN_RX2 + 2 * BTN_GAP + BTN_W,
        BTN_Y,
        &panel->cpu->running
    );

    /* Stop indicator */
    panel->indicators[1] = make_indicator_red(
        "CPU HALT",
        0,
        BTN_RX2 + 2 * BTN_GAP + BTN_W,
        BTN_Y + BTN_GAP + BTN_H,
        &panel->cpu->exit
    );

    /* Panel lock indicator */
    panel->indicators[2] = make_indicator_amber(
        "CTL LOCK",
        0,
        BTN_RX2 + 2 * BTN_GAP + BTN_W,
        BTN_Y + 2 * (BTN_GAP + BTN_H),
        &panel->locked
    );
    
    return 0;
}

void panel_do_render(void *ctx) {
    panel_ctx_t *panel = (panel_ctx_t *) ctx;
    
    pthread_mutex_lock(&panel->update_lock);

    for (int i = 0; i < NUM_INDICATORS; i++) {
        if (get_indicator_state(panel->indicators[i])) panel->updated = 1;
    }
    
    if (panel->updated) {
        /* --- Render --- */
        SDL_SetRenderDrawColor(panel->render, 0x1a, 0x1a, 0x1a, 255);
        SDL_RenderClear(panel->render);

        /* Row labels */
        SDL_Rect addr_label = {LABEL_X, ADDR_Y, LABEL_W - 5, SEG_H};
        draw_text_centered(panel->render, panel->font, "Addr",
                           &addr_label, 140, 140, 140);
        SDL_Rect data_label = {LABEL_X, DATA_Y, LABEL_W - 5, SEG_H};
        draw_text_centered(panel->render, panel->font, "Data",
                           &data_label, 140, 140, 140);

        /* Address display (amber) */
        draw_octal_row(panel->render, DISPLAY_X + 78, ADDR_Y,
                       ADDR_DIGITS, panel->addr_reg, 255, 170, 0);

        /* Data display (green) */
        draw_octal_row(panel->render, DISPLAY_X, DATA_Y,
                       DATA_DIGITS, panel->data_reg, 0, 220, 0);

        /* Buttons */
        for (int i = 0; i < NUM_BUTTONS; i++) {
            draw_button(panel->render, panel->new_buttons[i]);
        }

        /* Indicators */
        for (int i = 0; i < NUM_INDICATORS; i++) {
            draw_indicator(panel->render, panel->indicators[i]);
        }

        SDL_RenderPresent(panel->render);
        panel->updated = 0;
    }
    pthread_mutex_unlock(&panel->update_lock);
}

void panel_do_event(void *ctx, SDL_Event *event) {
    panel_ctx_t *panel = (panel_ctx_t *) ctx;
    /* --- Events --- */
    if (event->key.windowID == SDL_GetWindowID(panel->window) ||
        event->button.windowID == SDL_GetWindowID(panel->window)) {
        pthread_mutex_lock(&panel->update_lock);
        panel->updated = 1;
        pthread_mutex_unlock(&panel->update_lock);
        
        switch (event->type) {
            case SDL_MOUSEBUTTONDOWN:
                for (int i = 0; i < NUM_BUTTONS; i++) {
                    if (point_in_rect(event->button.x, event->button.y,
                        &panel->new_buttons[i]->box)) {
                        panel->new_buttons[i]->pressed = 1;
                        panel->new_buttons[i]->action_callback(panel, i);
                    }
                }
                break;

            case SDL_MOUSEBUTTONUP:
                for (int i = 0; i < NUM_BUTTONS; i++) {
                    panel->new_buttons[i]->pressed = 0;
                }
                break;

            case SDL_KEYDOWN: {
                for (int i = 0; i < NUM_BUTTONS; i++) {
                    if (panel->new_buttons[i]->hotkey == event->key.keysym.sym) {
                        panel->new_buttons[i]->pressed = 1;
                        panel->new_buttons[i]->action_callback(panel, i);
                    }
                }
            } break;

            case SDL_KEYUP: {
                for (int i = 0; i < NUM_BUTTONS; i++) {
                    if (panel->new_buttons[i]->hotkey == event->key.keysym.sym) {
                        panel->new_buttons[i]->pressed = 0;
                    }
                }
            } break;
        }
    }
}

void panel_do_destroy(void *ctx) {
    panel_ctx_t *panel = (panel_ctx_t *) ctx;
    TTF_CloseFont(panel->font);
    SDL_DestroyRenderer(panel->render);
    SDL_DestroyWindow(panel->window);
}

/* --- Init / destroy --- */

void destroy_panel(ist66_cu_t *cpu, int id) {
    panel_ctx_t *panel = (panel_ctx_t *) cpu->ioctx[id];
    panel->running = 0;
    // pthread_join(panel->thread, NULL);
    for (int i = 0; i < NUM_BUTTONS; i++) {
        destroy_button(panel->new_buttons[i]);
    }
    for (int i = 0; i < NUM_INDICATORS; i++) {
        destroy_button(panel->indicators[i]);
    }
    TTF_Quit();
    pthread_mutex_destroy(&panel->update_lock);
    free(panel);
}

void init_panel(ist66_cu_t *cpu, int id) {
    panel_ctx_t *ctx = calloc(sizeof(panel_ctx_t), 1);
    
    window_ctx_t panel_window = {
        .ctx = (void *) ctx,
        .do_init = panel_do_init,
        .do_render = panel_do_render,
        .do_event = panel_do_event,
        .do_destroy = panel_do_destroy
    };
    
    if (register_window(&(cpu->render_ctx), &panel_window)) {
        fprintf(stderr, "Panel: no windows left\n");
        free(ctx);
        return;
    }
    
    pthread_mutex_init(&ctx->update_lock, NULL);
    
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_panel;
    cpu->io[id] = NULL;

    ctx->running = 1;
    ctx->cpu = cpu;
    ctx->addr_reg = 0;
    ctx->data_reg = 0;
    ctx->locked = 0;

    // pthread_create(&(ctx->thread), NULL, panel_thread, ctx);
}

/*
 * End AI-generated code
 */
