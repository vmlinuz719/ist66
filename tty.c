#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>

#include "cpu.h"

typedef struct {
    ist66_cu_t *cpu;
    int id, irq;
    
    int sock_listener, sock_console;
    
    uint8_t buffer[256];
    uint8_t rd, wr, len;
    int was_full;
    pthread_mutex_t status_lock;
    pthread_cond_t status_empty_cond;
    
    pthread_t listener, reader, writer;
    
    int running, command, done;
} ist66_tty_t;

void push_char(void *vctx, uint8_t ch) {
    ist66_tty_t *ctx = (ist66_tty_t *) vctx;
    
    pthread_mutex_lock(&ctx->status_lock);
    
    while (ctx->was_full) {
        pthread_cond_wait(&ctx->status_empty_cond, &ctx->status_lock);
    }
    
    ctx->buffer[ctx->wr++] = ch;
    if (ctx->len++ == 255) ctx->was_full = 1;
    
    pthread_mutex_unlock(&ctx->status_lock);
}

int pop_char(void *vctx) {
    ist66_tty_t *ctx = (ist66_tty_t *) vctx;
    
    if (ctx->len == 0) return -1;
    
    int result;
    
    pthread_mutex_lock(&ctx->status_lock);
    
    result = ctx->buffer[ctx->rd++];
    if (ctx->len-- == 0) {
        ctx->was_full = 0;
        pthread_cond_signal(&ctx->status_empty_cond);
    }
    
    pthread_mutex_unlock(&ctx->status_lock);
    return result;
}