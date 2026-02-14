#include <stdint.h>
#include <stdio.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#include "cpu.h"
#include "alu.h"
#include "panel.h"

#define FONT_PATH "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Bold.ttf"
#define FONT_SIZE 18

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
#define BTN_X           20

#define NUM_BUTTONS     22    /* 0-7, Addr, Clr, Load, Ld+, Str, St+, LdAC, StAC, LdCt, StCt, Run, Halt, Step, StPC */

#define BTN_COLS        4
#define BTN_ROWS        4     /* row 0: 0-3, row 1: 4-7, row 2: Addr/Clr, row 3: mem ops */

/* Right-side columns for register and CPU control buttons */
#define BTN_RX          (BTN_X + BTN_COLS * (BTN_W + BTN_GAP) + BTN_GAP)
#define BTN_RX2         (BTN_RX + BTN_W + BTN_GAP)

#define SCREEN_WIDTH_DISPLAYS (DISPLAY_X + DATA_DIGITS * (SEG_W + SEG_GAP) + 40)
#define SCREEN_WIDTH_BUTTONS  (BTN_RX2 + BTN_W + 20)
#define SCREEN_WIDTH   ((SCREEN_WIDTH_DISPLAYS > SCREEN_WIDTH_BUTTONS) ? SCREEN_WIDTH_DISPLAYS : SCREEN_WIDTH_BUTTONS)
#define SCREEN_HEIGHT  (BTN_Y + BTN_ROWS * (BTN_H + BTN_GAP) + 12)

/* Button label strings */
static const char *btn_labels[NUM_BUTTONS] = {
    "0", "1", "2", "3", "4", "5", "6", "7",
    "Addr", "Clr", "Load", "Ld +", "Str", "St +",
    "LdAC", "StAC", "LdCt", "StCt",
    "Run", "Halt", "Step", "StPC"
};

/* --- Panel state --- */

typedef struct {
    int running;
    pthread_t thread;
    ist66_cu_t *cpu;

    SDL_Window *window;
    SDL_Renderer *render;

    uint32_t addr_reg;   /* 27-bit address */
    uint64_t data_reg;   /* 36-bit data */

    SDL_Rect buttons[NUM_BUTTONS];
} panel_ctx_t;

/* --- Drawing helpers --- */

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

static void draw_button(SDL_Renderer *r, SDL_Rect *rect, int pressed) {
    /* button background */
    if (pressed) {
        SDL_SetRenderDrawColor(r, 80, 80, 80, 255);
    } else {
        SDL_SetRenderDrawColor(r, 50, 50, 50, 255);
    }
    SDL_RenderFillRect(r, rect);

    /* button border */
    SDL_SetRenderDrawColor(r, 140, 140, 140, 255);
    SDL_RenderDrawRect(r, rect);
}

static int point_in_rect(int px, int py, SDL_Rect *r) {
    return px >= r->x && px < r->x + r->w &&
           py >= r->y && py < r->y + r->h;
}

static void do_button_action(panel_ctx_t *panel, int i) {
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
    } else if (i == 12) { /* Store */
        uint32_t a = panel->addr_reg & MASK_ADDR;
        if (a < panel->cpu->mem_size)
            panel->cpu->memory[a] = panel->data_reg & MASK_36;
    } else if (i == 13) { /* Store Next */
        panel->addr_reg = (panel->addr_reg + 1) & MASK_ADDR;
        uint32_t a = panel->addr_reg;
        if (a < panel->cpu->mem_size)
            panel->cpu->memory[a] = panel->data_reg & MASK_36;
    } else if (i == 14) { /* Ld AC */
        int r = panel->addr_reg & 0xF;
        panel->data_reg = panel->cpu->a[r] & MASK_36;
    } else if (i == 15) { /* St AC */
        int r = panel->addr_reg & 0xF;
        panel->cpu->a[r] = panel->data_reg & MASK_36;
    } else if (i == 16) { /* Ld Ct */
        int r = panel->addr_reg & 0x7;
        panel->data_reg = panel->cpu->c[r] & MASK_36;
    } else if (i == 17) { /* St Ct */
        int r = panel->addr_reg & 0x7;
        panel->cpu->c[r] = panel->data_reg & MASK_36;
    } else if (i == 18) { /* Run */
        if (!panel->cpu->running)
            start_cpu(panel->cpu, 0);
    } else if (i == 19) { /* Halt */
        if (panel->cpu->running)
            stop_cpu(panel->cpu);
    } else if (i == 20) { /* Step */
        if (!panel->cpu->running)
            start_cpu(panel->cpu, 1);
    } else if (i == 21) { /* StPC */
        set_pc(panel->cpu, panel->addr_reg & MASK_ADDR);
    }
}

/* Map key sym to button index, or -1 */
static int key_to_button(SDL_Keycode sym) {
    if (sym >= SDLK_0 && sym <= SDLK_7) return sym - SDLK_0;
    switch (sym) {
    case SDLK_a:         return 8;   /* Addr */
    case SDLK_c:         return 9;   /* Clr */
    case SDLK_l:         return 10;  /* Load */
    case SDLK_SEMICOLON: return 11;  /* Ld + */
    case SDLK_s:         return 12;  /* Str */
    case SDLK_d:         return 13;  /* St + */
    case SDLK_COMMA:     return 14;  /* Ld AC */
    case SDLK_z:         return 15;  /* St AC */
    case SDLK_PERIOD:    return 16;  /* Ld Ct */
    case SDLK_x:         return 17;  /* St Ct */
    default:             return -1;
    }
}

/* --- Main panel thread --- */

void *panel_thread(void *ctx) {
    panel_ctx_t *panel = (panel_ctx_t *) ctx;

    panel->window = SDL_CreateWindow(
        "RDC-700 Programmer's Panel",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN
    );
    if (!panel->window) {
        fprintf(stderr, "Panel: window creation failed: %s\n", SDL_GetError());
        return NULL;
    }

    panel->render = SDL_CreateRenderer(
        panel->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!panel->render) {
        SDL_DestroyWindow(panel->window);
        fprintf(stderr, "Panel: renderer creation failed: %s\n", SDL_GetError());
        return NULL;
    }

    /* Load font */
    if (TTF_Init() < 0) {
        fprintf(stderr, "Panel: TTF_Init failed: %s\n", TTF_GetError());
        SDL_DestroyRenderer(panel->render);
        SDL_DestroyWindow(panel->window);
        return NULL;
    }
    TTF_Font *font = TTF_OpenFont(FONT_PATH, FONT_SIZE);
    if (!font) {
        fprintf(stderr, "Panel: font load failed: %s\n", TTF_GetError());
        TTF_Quit();
        SDL_DestroyRenderer(panel->render);
        SDL_DestroyWindow(panel->window);
        return NULL;
    }

    /* Set up button positions: 4-column grid */
    for (int i = 0; i < 8; i++) {
        int col = i % BTN_COLS;
        int row = i / BTN_COLS;
        panel->buttons[i] = (SDL_Rect){
            BTN_X + col * (BTN_W + BTN_GAP),
            BTN_Y + row * (BTN_H + BTN_GAP),
            BTN_W, BTN_H
        };
    }
    /* Addr button — row 2, cols 0-1 */
    panel->buttons[8] = (SDL_Rect){
        BTN_X,
        BTN_Y + 2 * (BTN_H + BTN_GAP),
        2 * BTN_W + BTN_GAP, BTN_H
    };
    /* Clr button — row 2, cols 2-3 */
    panel->buttons[9] = (SDL_Rect){
        BTN_X + 2 * (BTN_W + BTN_GAP),
        BTN_Y + 2 * (BTN_H + BTN_GAP),
        2 * BTN_W + BTN_GAP, BTN_H
    };
    /* Memory operation buttons — row 3, one per column */
    for (int i = 0; i < 4; i++) {
        panel->buttons[10 + i] = (SDL_Rect){
            BTN_X + i * (BTN_W + BTN_GAP),
            BTN_Y + 3 * (BTN_H + BTN_GAP),
            BTN_W, BTN_H
        };
    }
    /* Register buttons — right-side column, rows 0-3 */
    for (int i = 0; i < 4; i++) {
        panel->buttons[14 + i] = (SDL_Rect){
            BTN_RX,
            BTN_Y + i * (BTN_H + BTN_GAP),
            BTN_W, BTN_H
        };
    }
    /* CPU control buttons — second right-side column, rows 0-3 */
    for (int i = 0; i < 4; i++) {
        panel->buttons[18 + i] = (SDL_Rect){
            BTN_RX2,
            BTN_Y + i * (BTN_H + BTN_GAP),
            BTN_W, BTN_H
        };
    }

    int pressed_btn = -1;

    while (panel->running) {
        /* --- Render --- */
        SDL_SetRenderDrawColor(panel->render, 0x1a, 0x1a, 0x1a, 255);
        SDL_RenderClear(panel->render);

        /* Row labels */
        SDL_Rect addr_label = {LABEL_X, ADDR_Y, LABEL_W - 5, SEG_H};
        draw_text_centered(panel->render, font, "ADDR",
                           &addr_label, 255, 170, 0);
        SDL_Rect data_label = {LABEL_X, DATA_Y, LABEL_W - 5, SEG_H};
        draw_text_centered(panel->render, font, "DATA",
                           &data_label, 0, 220, 0);

        /* Address display (amber) */
        draw_octal_row(panel->render, DISPLAY_X, ADDR_Y,
                       ADDR_DIGITS, panel->addr_reg, 255, 170, 0);

        /* Data display (green) */
        draw_octal_row(panel->render, DISPLAY_X, DATA_Y,
                       DATA_DIGITS, panel->data_reg, 0, 220, 0);

        /* Buttons */
        for (int i = 0; i < NUM_BUTTONS; i++) {
            draw_button(panel->render, &panel->buttons[i], pressed_btn == i);
            draw_text_centered(panel->render, font, btn_labels[i],
                               &panel->buttons[i], 200, 200, 200);
        }

        SDL_RenderPresent(panel->render);

        /* --- Events --- */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_MOUSEBUTTONDOWN:
                for (int i = 0; i < NUM_BUTTONS; i++) {
                    if (point_in_rect(event.button.x, event.button.y,
                                      &panel->buttons[i])) {
                        pressed_btn = i;
                        do_button_action(panel, i);
                        break;
                    }
                }
                break;

            case SDL_MOUSEBUTTONUP:
                pressed_btn = -1;
                break;

            case SDL_KEYDOWN: {
                int btn = key_to_button(event.key.keysym.sym);
                if (btn >= 0) {
                    pressed_btn = btn;
                    do_button_action(panel, btn);
                }
                break;
            }

            case SDL_KEYUP: {
                int btn = key_to_button(event.key.keysym.sym);
                if (btn >= 0 && pressed_btn == btn)
                    pressed_btn = -1;
                break;
            }

            case SDL_QUIT:
                panel->running = 0;
                break;
            }
        }
    }

    TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(panel->render);
    SDL_DestroyWindow(panel->window);

    return NULL;
}

/* --- Init / destroy --- */

void destroy_panel(ist66_cu_t *cpu, int id) {
    panel_ctx_t *panel = (panel_ctx_t *) cpu->ioctx[id];
    panel->running = 0;
    pthread_join(panel->thread, NULL);
    free(panel);
}

void init_panel(ist66_cu_t *cpu, int id) {
    panel_ctx_t *ctx = calloc(sizeof(panel_ctx_t), 1);
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_panel;
    cpu->io[id] = NULL;

    ctx->running = 1;
    ctx->cpu = cpu;
    ctx->addr_reg = 0;
    ctx->data_reg = 0;

    pthread_create(&(ctx->thread), NULL, panel_thread, ctx);
}
