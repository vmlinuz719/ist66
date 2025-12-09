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

#define TELNET_SE 0xF0
#define TELNET_EOR 0xF1
#define TELNET_SB 0xFA
#define TELNET_IAC 0xFF

enum telnet_telnet_state {
    TN_NORMAL,
    TN_COMMAND,
    TN_SUBNEG
};

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
    
    while (ctx->len == 255) {
        pthread_cond_wait(&ctx->status_empty_cond, &ctx->status_lock);
    }
    
    ctx->buffer[ctx->wr++] = ch;
    ctx->len++;
    
    pthread_mutex_unlock(&ctx->status_lock);
}

int pop_char(void *vctx) {
    ist66_tty_t *ctx = (ist66_tty_t *) vctx;
    
    if (ctx->len == 0) return -1;
    
    int result;
    
    pthread_mutex_lock(&ctx->status_lock);
    
    result = ctx->buffer[ctx->rd++];
    if (ctx->len-- == 255) {
        pthread_cond_signal(&ctx->status_empty_cond);
    }
    
    pthread_mutex_unlock(&ctx->status_lock);
    return result;
}

void *tty_reader(void *vctx) {
    ist66_tty_t *ctx = (ist66_tty_t *) vctx;
    
    uint8_t buf[256];
    int telnet_state = TN_NORMAL;
    
    while (ctx->running) {
        int nrecv = recv(ctx->sock_console, &buf, sizeof(buf), 0);
        
        if (nrecv <= 0) {
            ctx->running = 0;
            return NULL;
        }
        
        for (int i = 0; i < nrecv; i++) {
            switch (telnet_state) {
                case TN_NORMAL:
                    if (buf[i] == TELNET_IAC) {
                        telnet_state = TN_COMMAND;
                    } else {
                        push_char(ctx, buf[i]);
                    }
                    break;
                
                case TN_COMMAND:
                    if (buf[i] == 0xFF) {
                        push_char(ctx, buf[i]);
                        telnet_state = TN_NORMAL;
                    } else if (buf[i] == TELNET_SB) {
                        telnet_state = TN_SUBNEG;
                    } else {
                        telnet_state = TN_NORMAL;
                    }
                    break;
                
                case TN_SUBNEG:
                    if (buf[i] == TELNET_SE) {
                        telnet_state = TN_NORMAL;
                    } else {
                        // continue consuming subneg bytes
                    }
                    break;
            }
        }
    }
    
    return NULL;
}