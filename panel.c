#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#include "cpu.h"
#include "panel.h"

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 256
#define PANEL_ROWS 8
#define PANEL_VPAD 10
#define PANEL_HPAD 2
#define LED_HEIGHT 16
#define LED_WIDTH  16

typedef struct {
    int running;
    pthread_t thread;
    ist66_cu_t *cpu;
    
    SDL_Window *window;
    SDL_Renderer *render;
} panel_ctx_t;

void *panel_thread(void *ctx) {
    panel_ctx_t *panel_ctx = (panel_ctx_t *) ctx;

    panel_ctx->window = SDL_CreateWindow(
        "RDC700",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN
    );
    
    if (panel_ctx->window == NULL) {
        return NULL;
    }
    
    panel_ctx->render = SDL_CreateRenderer
            (panel_ctx->window, -1, 
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (panel_ctx->render == NULL) {
        SDL_DestroyRenderer(panel_ctx->render);
        return NULL;
    }
    
    uint64_t panel_rows[PANEL_ROWS];
    int start_led[PANEL_ROWS] = {
        28, 28, 28, 28, 0, 28, 60, 0
    };
    int end_led[PANEL_ROWS] = {
        64, 64, 64, 64, 0, 64, 64, 0
    };
    int row_color[PANEL_ROWS][3] = {
        {255, 149, 66},
        {255, 149, 66},
        {255, 149, 66},
        {255, 149, 66},
        {0, 0, 0},
        {255, 66, 66},
        {255, 66, 66},
    };
    
    int selection = 0;
    while (panel_ctx->running) {
        SDL_RenderClear(panel_ctx->render);
        
        
        panel_rows[0] = panel_ctx->cpu->c[0];
        panel_rows[1] = panel_ctx->cpu->inst;
        panel_rows[2] = panel_ctx->cpu->c[1];
        panel_rows[3] = panel_ctx->cpu->c[6];
        panel_rows[5] = panel_ctx->cpu->a[selection];
        panel_rows[6] = selection;
        
        for (int j = 0; j < PANEL_ROWS; j++) {
            for (int i = start_led[j]; i < end_led[j]; i++) {
                SDL_Rect rect;
                rect.x = PANEL_VPAD + (PANEL_HPAD + LED_WIDTH) * i;
                rect.y = PANEL_VPAD + (LED_HEIGHT + PANEL_VPAD) * j;
                rect.w = LED_WIDTH;
                rect.h = LED_HEIGHT;
                if (panel_rows[j] & (0x8000000000000000 >> i)) {
                    SDL_SetRenderDrawColor
                            (panel_ctx->render,
                            row_color[j][0],
                            row_color[j][1],
                            row_color[j][2],
                            255);
                    SDL_RenderFillRect(panel_ctx->render, &rect);
                } else {
                    SDL_SetRenderDrawColor(panel_ctx->render, 64, 64, 64, 255);
                    SDL_RenderDrawRect(panel_ctx->render, &rect);
                }
                
            }
        }
        
        SDL_SetRenderDrawColor(panel_ctx->render, 0, 0, 0, 255);

        SDL_RenderPresent(panel_ctx->render);
        
        SDL_Event event;
        while( SDL_PollEvent( &event ) ){
            switch( event.type ){
                /* Look for a keypress */
                case SDL_KEYDOWN:
                    /* Check the SDLKey values and move change the coords */
                    switch( event.key.keysym.scancode ){
                        /*
                        case SDLK_LEFT:
                            alien_x -= 1;
                            break;
                        case SDLK_RIGHT:
                            alien_x += 1;
                            break;
                        */
                        case SDL_SCANCODE_UP:
                            if (selection < 15) selection++;
                            break;
                        case SDL_SCANCODE_T:
                            panel_ctx->cpu->throttle ^= 1;
                            break;
                        case SDL_SCANCODE_DOWN:
                            if (selection > 0) selection--;
                            break;
                        default:
                            break;
                    }
                    break;
                case SDL_QUIT:
                    panel_ctx->running = 0;
                    break;
            }
        }
    }

    SDL_DestroyRenderer(panel_ctx->render);    
    SDL_DestroyWindow(panel_ctx->window);
    
    return NULL;
}

void destroy_panel(ist66_cu_t *cpu, int id) {
    panel_ctx_t *panel_ctx = (panel_ctx_t *) cpu->ioctx[id];
    panel_ctx->running = 0;
    pthread_join(panel_ctx->thread, NULL);
    free(panel_ctx);
}

void init_panel(ist66_cu_t *cpu, int id) {
    panel_ctx_t *ctx = calloc(sizeof(panel_ctx_t), 1);
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_panel;
    cpu->io[id] = NULL;
    

    ctx->running = 1;
    
    ctx->cpu = cpu;
    

    pthread_create(&(ctx->thread), NULL, panel_thread, ctx);
}
