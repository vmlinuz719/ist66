#ifndef _SDLCTX_
#define _SDLCTX_

#include <SDL2/SDL.h>
#include <pthread.h>

#define MAX_WINDOWS 4

typedef struct {
    void *ctx;
    
    int (*do_init)(void *);
    void (*do_render)(void *);
    void (*do_event)(void *, SDL_Event *);
    void (*do_destroy)(void *);
} window_ctx_t;

typedef struct {
    window_ctx_t window_ctx[MAX_WINDOWS];
    int num_windows;
    
    int loop_running;
    pthread_t thread;
} render_loop_ctx_t;

int register_window(render_loop_ctx_t *, window_ctx_t *);
void start_render(render_loop_ctx_t *);
void kill_render(render_loop_ctx_t *);

#endif