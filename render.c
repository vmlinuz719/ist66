#include <SDL2/SDL.h>
#include <pthread.h>

#include "sdlctx.h"

int register_window(render_loop_ctx_t *ctx, window_ctx_t *w) {
    if (ctx->num_windows >= MAX_WINDOWS) return -1;
    
    window_ctx_t *tgt = &(ctx->window_ctx[ctx->num_windows++]);
    *tgt = *w;
    
    return 0;
}

void *render_loop(void *vctx) {
    render_loop_ctx_t *ctx = (render_loop_ctx_t *) vctx;
    
    if (SDL_Init(SDL_INIT_EVERYTHING)) return NULL;
    
    int i = 0;
    while (i < ctx->num_windows) {
        if (ctx->window_ctx[i].do_init(ctx->window_ctx[i].ctx))
            ctx->window_ctx[i] = ctx->window_ctx[ctx->num_windows--];
        else i++;
    }
    
    if (ctx->num_windows == 0) return NULL;
    
    while (ctx->loop_running) {
        for (i = 0; i < ctx->num_windows; i++) {
            ctx->window_ctx[i].do_render(ctx->window_ctx[i].ctx);
        }
        
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            for (i = 0; i < ctx->num_windows; i++) {
                if (ctx->window_ctx[i].do_event != NULL) {
                    ctx->window_ctx[i].do_event
                        (ctx->window_ctx[i].ctx, &event);
                }
            }
        }
    }
    
    for (i = 0; i < ctx->num_windows; i++) {
        ctx->window_ctx[i].do_destroy(ctx->window_ctx[i].ctx);
    }
    
    return NULL;
}

void start_render(render_loop_ctx_t *ctx) {
    ctx->loop_running = 1;
    pthread_create(&(ctx->thread), NULL, render_loop, ctx);
}

void kill_render(render_loop_ctx_t *ctx) {
    if (ctx->loop_running) {
        ctx->loop_running = 0;
        pthread_join(ctx->thread, NULL);
    }
}