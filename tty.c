#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

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
    
    int listening, running, command, done;
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
                    fprintf(stderr, "%hhu\n", buf[i]);
                    if (buf[i] == 0xFF) {
                        push_char(ctx, buf[i]);
                        telnet_state = TN_NORMAL;
                    } else if (buf[i] == TELNET_SB) {
                        telnet_state = TN_SUBNEG;
                    } else if (buf[i] < 250) {
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

void *tty_listener(void *vctx) {
    ist66_tty_t *ctx = (ist66_tty_t *) vctx;
    
    listen(ctx->sock_listener, 1);
    
    while (1) {
        struct sockaddr_in client;
        socklen_t c = sizeof(client);
        
        int new_connection =
            accept(ctx->sock_listener, (struct sockaddr *) &client, &c);
        
        if (new_connection < 0) {
            // connection failed
            ctx->listening = 0;
            return NULL;
        }
        
        if (!(ctx->running)) {
            uint8_t mode[] = {
                255, 251, 1, 255, 251, 3
            };
            send(
                new_connection,
                mode,
                sizeof(mode),
                0
            );
            
            ctx->sock_console = new_connection;
            ctx->running = 1;
            pthread_create
                (&ctx->reader, NULL, tty_reader, ctx);
            // create writer too
            fprintf(stderr, "/DEV-I-UNIT %04o TTY CONNECT\n", ctx->id);
        } else {
            static char *msg = "/TTY-E-BUSY\n";
            send(new_connection, msg, sizeof(msg), 0);
            close(new_connection);
        }
    }
    
    return NULL;
}

uint64_t tty_io(
    void *vctx,
    uint64_t data,
    int ctl,
    int transfer
) {
    // ist66_tty_t *ctx = (ist66_tty_t *) vctx;
    // ist66_cu_t *cpu = ctx->cpu;
    
    // stub
    
    return 0;
}

void destroy_tty(ist66_cu_t *cpu, int id) {
    ist66_tty_t *ctx = (ist66_tty_t *) cpu->ioctx[id];
    
    if (ctx->running) {
        pthread_cancel(ctx->reader);
        // pthread_cancel(ctx->writer);
        close(ctx->sock_console);
    }
    
    if (ctx->listening) {
        pthread_cancel(ctx->listener);
    }
    
    close(ctx->sock_listener);
    
    pthread_mutex_destroy(&ctx->status_lock);
    pthread_cond_destroy(&ctx->status_empty_cond);
    free(ctx);
    
    fprintf(stderr, "/DEV-I-UNIT %04o TTY CLOSED\n", id);
}

void init_tty(ist66_cu_t *cpu, int id, int irq, int port) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) {
        fprintf(stderr, "/DEV-E-UNIT %04o TTY CREATE FAIL\n", id);
        return;
    }
    
    struct sockaddr_in server_in;
    server_in.sin_family = AF_INET;
    server_in.sin_addr.s_addr = INADDR_ANY;
    server_in.sin_port = htons(port);

    if (bind(server_sock, (struct sockaddr *)&server_in, sizeof(server_in)) < 0) {
        fprintf(stderr, "/DEV-E-UNIT %04o TTY BIND FAIL\n", id);
        close(server_sock);
        return;
    }
    
    ist66_tty_t *ctx = calloc(sizeof(ist66_tty_t), 1);
    cpu->ioctx[id] = ctx;
    cpu->io_destroy[id] = destroy_tty;
    cpu->io[id] = tty_io;
    
    ctx->cpu = cpu;
    ctx->id = id;
    ctx->irq = irq;
    ctx->sock_listener = server_sock;
    
    pthread_mutex_init(&ctx->status_lock, NULL);
    pthread_cond_init(&ctx->status_empty_cond, NULL);
    
    ctx->listening = 1;
    
    pthread_create(&ctx->listener, NULL, tty_listener, ctx);
    fprintf(stderr, "/DEV-I-UNIT %04o TTY IRQ %02o %d\n", id, irq, port);
}