#include <stdint.h>
#include <stdio.h>
#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include "cpu.h"
#define MASK_36 0xFFFFFFFFFL

#define BISHOP_WIDTH 512
#define BISHOP_HEIGHT 480
#define BISHOP_OVERSCAN 32

#define RGBA_BLACK 0xFF
#define RGBA_GREEN 0x00FF00FF

#define MIN(x, y) ((x) < (y) ? (x) : (y))

typedef struct {
    uint8_t pixels[BISHOP_WIDTH * (BISHOP_HEIGHT + BISHOP_OVERSCAN)];
    
    int x, y, x1, y1, updated, command;
    pthread_mutex_t update_lock, cmd_lock;
    pthread_cond_t cmd_cond;


    int running, done;
    pthread_t thread, dma_thread;
    ist66_cu_t *cpu;
    
    uint64_t dma_addr;
    uint64_t pattern;
    uint64_t rect; // y, x, w, h (9 bits each)
    uint64_t base; // iy, ix, y0, x0 (9 bits each)
    uint64_t scroll;

    SDL_Window *window;
    SDL_Renderer *render;
} bishop_ctx_t;

uint64_t bishop_dma_read(
    bishop_ctx_t *bishop, uint64_t address,
    int x, int y, int w, int h
) {
    uint64_t ea = address & MASK_ADDR;
    uint64_t sh = address >> 27;
    uint64_t bs = 1;

    for (int cur_y = y; cur_y < y + h; cur_y++) {
        for (int cur_x = x; cur_x < x + w; cur_x++) {
            sh -= bs;
            if (sh > 36) {
                sh = (36 - bs) & 0x3F;
                ea = (ea + 1) & MASK_ADDR;
            }

            if (
                (ea >= bishop->cpu->mem_size)
                /*
                || ((cur_y * BISHOP_WIDTH + cur_x)
                >= (BISHOP_WIDTH * (BISHOP_HEIGHT + BISHOP_OVERSCAN)))
                */
            ) {
                pthread_mutex_lock(&bishop->update_lock);
                bishop->updated = 1;
                bishop->x = 0;
                bishop->y = 0;
                bishop->x1 = BISHOP_WIDTH - 1;
                bishop->y1 = BISHOP_HEIGHT - 1;
                pthread_mutex_unlock(&bishop->update_lock);
                return MASK_36;
            }

            uint64_t data = bishop->cpu->memory[ea];
            data &= MASK_36;
            data >>= sh;
            data &= (1L << bs) - 1;

            bishop->pixels[
                ((cur_y + (bishop->scroll & 0x1FF))
                    % (BISHOP_HEIGHT + BISHOP_OVERSCAN)) * BISHOP_WIDTH
                + (cur_x % BISHOP_WIDTH)
            ] = data;
        }
    }

    pthread_mutex_lock(&bishop->update_lock);
    
    bishop->updated = 1;
    if (x < bishop->x) bishop->x = x;
    if (y < bishop->y) bishop->y = y;
    if (x + w - 1 > bishop->x1) bishop->x1 = x + w - 1;
    if (y + h - 1 > bishop->y1) bishop->y1 = y + h - 1;

    pthread_mutex_unlock(&bishop->update_lock);

    return ea | (sh << 27);
}

void bishop_dma_pattern(
    bishop_ctx_t *bishop, uint64_t pattern,
    int x, int y, int w, int h
) {
    uint64_t sh = 36;

    for (int cur_y = y; cur_y < y + h; cur_y++) {
        for (int cur_x = x; cur_x < x + w; cur_x++) {
            sh -= 1;
            if (sh > 35) {
                sh = 35;
            }

            uint64_t data = (pattern >> sh) & 1;

            bishop->pixels[
                ((cur_y + (bishop->scroll & 0x1FF))
                    % (BISHOP_HEIGHT + BISHOP_OVERSCAN)) * BISHOP_WIDTH
                + (cur_x % BISHOP_WIDTH)
            ] = data;
        }
    }

    pthread_mutex_lock(&bishop->update_lock);
    
    bishop->updated = 1;
    if (x < bishop->x) bishop->x = x;
    if (y < bishop->y) bishop->y = y;
    if (x + w - 1 > bishop->x1) bishop->x1 = x + w - 1;
    if (y + h - 1 > bishop->y1) bishop->y1 = y + h - 1;

    pthread_mutex_unlock(&bishop->update_lock);
}

void bishop_scroll(bishop_ctx_t *bishop, uint64_t new_scroll) {
    pthread_mutex_lock(&bishop->update_lock);
    bishop->scroll = new_scroll;
    bishop->updated = 1;
    bishop->x = 0;
    bishop->y = 0;
    bishop->x1 = BISHOP_WIDTH - 1;
    bishop->y1 = BISHOP_HEIGHT - 1;
    pthread_mutex_unlock(&bishop->update_lock);
}

void *bishop_dma(void *vctx) {
    bishop_ctx_t *ctx = (bishop_ctx_t *) vctx;
    
    while (ctx->running) {
        pthread_mutex_lock(&ctx->cmd_lock);
        while (!ctx->command) {
            pthread_cond_wait(&ctx->cmd_cond, &ctx->cmd_lock);
            
        }
        
        int command = ctx->command;
        pthread_mutex_unlock(&ctx->cmd_lock);
        
        if (command == -1) {
            ctx->running = 0;
        }
        
        else if (command == 1 || command == 2) {
            uint64_t rect = ctx->rect;
            uint64_t base = ctx->base;
            
            int read_w = (rect & 0x1FF);
            rect >>= 9;
            int read_h = (rect & 0x1FF);
            rect >>= 9;
            int read_x = ((rect & 0x1FF) + (base & 0x1FF)) & 0x1FF;
            rect >>= 9;
            base >>= 9;
            int read_y = ((rect & 0x1FF) + (base & 0x1FF)) & 0x1FF;
            
            if (command == 1) {
                uint64_t new_addr = 
                    bishop_dma_read(
                        ctx,
                        ctx->dma_addr,
                        read_x,
                        read_y,
                        read_w,
                        read_h
                    );
                if (new_addr == MASK_36) {
                    // error
                    pthread_mutex_lock(&ctx->cmd_lock);
                    ctx->command = 0;
                    ctx->done = 1;
                    pthread_mutex_unlock(&ctx->cmd_lock);
                    continue;
                }
                
                ctx->dma_addr = new_addr;
            } else if (command == 2) {
                bishop_dma_pattern(
                    ctx,
                    ctx->pattern,
                    read_x,
                    read_y,
                    read_w,
                    read_h
                );
            }
            
            base = ctx->base;
            uint64_t new_x0 = 
                ((base & 0x1FF) + ((base >> 18) & 0x1FF));
            uint64_t new_y0 = 
                (((base >> 9) & 0x1FF) + ((base >> 27) & 0x1FF)) & 0x1FF;
            ctx->base = ((ctx->base & 0777777000000) | (new_y0 << 9)) + new_x0;
            
            pthread_mutex_lock(&ctx->cmd_lock);
            ctx->command = 0;
            ctx->done = 1;
            pthread_mutex_unlock(&ctx->cmd_lock);
        }
    }
    
    return NULL;
}

uint64_t bishop_io(
    void *vctx,
    uint64_t data,
    int ctl,
    int transfer
) {
    bishop_ctx_t *ctx = (bishop_ctx_t *) vctx;
    // ist66_cu_t *cpu = ctx->cpu;
    
    switch (transfer) {
        case 1: ctx->dma_addr = data & MASK_36; break;
        case 3: ctx->pattern = data & MASK_36; break;
        case 5: ctx->rect = data & MASK_36; break;
        case 7: ctx->base = data & MASK_36; break;
        case 9: bishop_scroll(ctx, data & MASK_36); break;
    }
    
    if (transfer != 14) {
        switch (ctl) {
            case 1: {
                if (transfer == 1) {
                    pthread_mutex_lock(&ctx->cmd_lock);
                    ctx->command = 1;
                    if (ctx->done) {
                        ctx->done = 0;
                        // intr_release(cpu, ctx->irq);
                    }
                    pthread_cond_signal(&ctx->cmd_cond);
                    pthread_mutex_unlock(&ctx->cmd_lock);
                } else if (transfer == 3) {
                    pthread_mutex_lock(&ctx->cmd_lock);
                    ctx->command = 2;
                    if (ctx->done) {
                        ctx->done = 0;
                        // intr_release(cpu, ctx->irq);
                    }
                    pthread_cond_signal(&ctx->cmd_cond);
                    pthread_mutex_unlock(&ctx->cmd_lock);
                }
            } break;
            case 2: {
                pthread_mutex_lock(&ctx->cmd_lock);
                ctx->command = 0;
                if (ctx->done) {
                    ctx->done = 0;
                    // intr_release(cpu, ctx->irq);
                }
                pthread_mutex_unlock(&ctx->cmd_lock);
            } break;
            case 3: {
                uint64_t new_scroll =
                    ((ctx->scroll & 0x1FF)
                    + ((ctx->scroll >> 9) & 0x1FF))
                    & 0x1FF;
                new_scroll |= ctx->scroll & 0x3FE00;
                bishop_scroll(ctx, new_scroll);
            } break;
        }
    }
    
    if (transfer == 14) {
        int status = (ctx->done << 1) | ((!!(ctx->command)) & 1);
        return (uint64_t) status;
    }
    
    switch (transfer) {
        case 0: return ctx->dma_addr;
        case 2: return ctx->pattern;
        case 4: return ctx->rect;
        case 6: return ctx->base;
        case 8: return ctx->scroll;
        default: return 0;
    }
    
}

void *bishop_thread(void *ctx) {
    bishop_ctx_t *bishop = (bishop_ctx_t *) ctx;
    
    bishop->window = SDL_CreateWindow(
        "You're watching Bishop TV",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        2 * BISHOP_WIDTH, 2 * BISHOP_HEIGHT,
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
    
    SDL_RenderSetLogicalSize(bishop->render, BISHOP_WIDTH, BISHOP_HEIGHT);
    
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
            int captured_scroll = bishop->scroll & 0x1FF;
            int captured_x1 = MIN(bishop->x1, BISHOP_WIDTH - 1);
            int captured_y1 = MIN(bishop->y1, BISHOP_HEIGHT - 1);
            
            bishop->x = BISHOP_WIDTH;
            bishop->y = BISHOP_HEIGHT;
            bishop->x1 = -1;
            bishop->y1 = -1;
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
            
            int write_y = ((captured_y + captured_scroll)
                % (BISHOP_HEIGHT + BISHOP_OVERSCAN));
            int write_offset = 0;
            
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int write_base = write_y * BISHOP_WIDTH + captured_x;
                    int pixel = (bishop->pixels[write_base + x])
                        ? RGBA_GREEN
                        : RGBA_BLACK;
                    pixbuf[write_offset++] = pixel;
                }
                write_y += 1;
                write_y %= (BISHOP_HEIGHT + BISHOP_OVERSCAN);
            }
            
            SDL_UpdateTexture(
                tex, &updated_rect, pixbuf, updated_rect.w * 4
            );
        }

        SDL_RenderClear(bishop->render);
        SDL_RenderCopy(bishop->render, tex, NULL, NULL);
        // vblank here
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
    bishop->command = -1;
    pthread_cond_signal(&bishop->cmd_cond);
    pthread_join(bishop->dma_thread, NULL);
    pthread_cond_destroy(&bishop->cmd_cond);
    pthread_mutex_destroy(&bishop->cmd_lock);
    pthread_mutex_destroy(&bishop->update_lock);

    free(bishop);
    fprintf(stderr, "TV2: %04o deinitialized\n", id);
}

void init_bishop(ist66_cu_t *cpu, int id) {
    bishop_ctx_t *ctx = calloc(sizeof(bishop_ctx_t), 1);
    
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_bishop;
    cpu->io[id] = bishop_io;

    ctx->cpu = cpu;
    
    ctx->x1 = BISHOP_WIDTH - 1;
    ctx->y1 = BISHOP_HEIGHT - 1;
    pthread_mutex_init(&ctx->update_lock, NULL);
    pthread_mutex_init(&ctx->cmd_lock, NULL);
    pthread_cond_init(&ctx->cmd_cond, NULL);

    ctx->running = 1;
    ctx->updated = 1;
    ctx->done = 1;
    pthread_create(&(ctx->thread), NULL, bishop_thread, ctx);
    pthread_create(&(ctx->dma_thread), NULL, bishop_dma, ctx);
    fprintf(stderr, "TV2: %04o\n", id);
}
