#include <stdint.h>
#include <stdio.h>
#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include "cpu.h"

#define BISHOP_WIDTH 512
#define BISHOP_HEIGHT 480
#define BISHOP_OVERSCAN 32

#define RGBA_BLACK 0xFF
#define RGBA_GREEN 0x00FF00FF

#define MIN(x, y) ((x) < (y) ? (x) : (y))

typedef struct {
    uint8_t pixels[BISHOP_WIDTH * (BISHOP_HEIGHT + BISHOP_OVERSCAN)];
    
    int x, y, x1, y1, updated;
    pthread_mutex_t update_lock;
    
    int running;
    pthread_t thread;
    // ist66_cu_t *cpu;

    SDL_Window *window;
    SDL_Renderer *render;
} bishop_ctx_t;

void *bishop_thread(void *ctx) {
    bishop_ctx_t *bishop = (bishop_ctx_t *) ctx;
    
    bishop->window = SDL_CreateWindow(
        "You're watching Bishop TV",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        BISHOP_WIDTH, BISHOP_HEIGHT,
        SDL_WINDOW_SHOWN
    );
    if (!bishop->window) {
        fprintf(stderr, "Bishop: window creation failed: %s\n", SDL_GetError());
        return NULL;
    }

    bishop->render = SDL_CreateRenderer(
        bishop->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!bishop->render) {
        SDL_DestroyWindow(bishop->window);
        fprintf(stderr, "Bishop: renderer creation failed: %s\n", SDL_GetError());
        return NULL;
    }
    
    SDL_Texture *tex = SDL_CreateTexture(
        bishop->render,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        BISHOP_WIDTH,
        BISHOP_HEIGHT
    );
    
    int pixbuf[BISHOP_WIDTH * BISHOP_HEIGHT];
    
    while(bishop->running) {
        if (bishop->updated) {
            pthread_mutex_lock(&bishop->update_lock);
            
            int captured_x = bishop->x;
            int captured_y = bishop->y;
            int captured_x1 = MIN(bishop->x1, BISHOP_WIDTH - 1);
            int captured_y1 = MIN(bishop->y1, BISHOP_HEIGHT - 1);
            
            bishop->x = -1;
            bishop->y = -1;
            bishop->x1 = 0;
            bishop->y1 = 0;
            bishop->updated = 0;
            
            pthread_mutex_unlock(&bishop->update_lock);
            
            int width = captured_x1 - captured_x + 1;
            int height = captured_y1 - captured_y + 1;
            SDL_Rect updated_rect = {
                .x = captured_x,
                .y = captured_y,
                .w = width,
                .h = height
            };
            
            int write_base = (captured_y * BISHOP_WIDTH) + captured_x;
            int write_offset = 0;
            
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int pixel = (bishop->pixels[write_base + x])
                        ? RGBA_GREEN
                        : RGBA_BLACK;
                    pixbuf[write_offset++] = pixel;
                }
                write_base += BISHOP_WIDTH;
            }
            
            SDL_UpdateTexture(
                tex, &updated_rect, pixbuf, updated_rect.w * 4
            );
        }

        SDL_RenderClear(bishop->render);
        SDL_RenderCopy(bishop->render, tex, NULL, NULL);
        SDL_RenderPresent(bishop->render);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {};
    }
    
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(bishop->render);
    SDL_DestroyWindow(bishop->window);
    return NULL;
}

void destroy_bishop(ist66_cu_t *cpu, int id) {
    bishop_ctx_t *bishop = cpu->ioctx[id];
    bishop->running = 0;
    pthread_join(bishop->thread, NULL);
    pthread_mutex_destroy(&bishop->update_lock);

    free(bishop);
}

void init_bishop(ist66_cu_t *cpu, int id) {
    bishop_ctx_t *ctx = calloc(sizeof(bishop_ctx_t), 1);
    
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_bishop;
    cpu->io[id] = NULL;
    
    ctx->x1 = BISHOP_WIDTH;
    ctx->y1 = BISHOP_HEIGHT;
    pthread_mutex_init(&ctx->update_lock, NULL);

    ctx->running = 1;
    ctx->updated = 1;
    pthread_create(&(ctx->thread), NULL, bishop_thread, ctx);
}
