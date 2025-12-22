#include <stdio.h>
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

#define ENABLED     1
#define INTR_ANY    2
#define INTR_ESC    4
#define INTR_RET    8
#define DESTRUCT    16
#define BSNOECHO    32
#define ECHO_RET    64
#define ECHO_TAB    128
#define ECHO_ALL    256

#define DEFAULTS    (ECHO_ALL | ECHO_TAB | ECHO_RET | INTR_RET)

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
    uint8_t rd, wr, len, threshold, send;
    int was_full;
    pthread_mutex_t status_lock, intr_lock;
    pthread_cond_t write_cond;
    
    pthread_t listener, reader, writer;
    
    uint16_t control;
    
    int listening, running, writing, command, done;
} ist66_tty_t;

void push_char(void *vctx, uint8_t ch) {
    ist66_tty_t *ctx = (ist66_tty_t *) vctx;
    
    pthread_mutex_lock(&ctx->status_lock);
    
    // fprintf(stderr, "Debug: %02hhX\n", ch);
    
    if (ctx->len < 255 && (ctx->control & ENABLED)) {
        ctx->buffer[ctx->wr++] = ch;
        ctx->len++;
        
        if (
            (ctx->control & ECHO_ALL) // TODO: check printable?
            || ((ctx->control & ECHO_TAB) && (ch == '\t'))
            || ((ctx->control & ECHO_RET) && (ch == 0x0A || ch == 0x0D))
        ) {
            send(ctx->sock_console, &ch, 1, 0);
        }
        
        if (
            (ctx->control & INTR_ANY)
            || ((ctx->control & INTR_ESC) && (ch == 0x1B))
            || ((ctx->control & INTR_RET) && (ch == 0x0A))
            || ((ctx->threshold) && (ctx->len >= ctx->threshold))
        ) {
            pthread_mutex_lock(&ctx->intr_lock);
            
            if (!ctx->done) {
                ctx->done = 1;
                intr_assert(ctx->cpu, ctx->irq);
            }
            
            pthread_mutex_unlock(&ctx->intr_lock);
        }
    } else {
        static char bell = '\a';
        send(ctx->sock_console, &bell, 1, 0);
    }
    
    pthread_mutex_unlock(&ctx->status_lock);
}

int pop_char(void *vctx) {
    ist66_tty_t *ctx = (ist66_tty_t *) vctx;
    
    if (ctx->len == 0) return -1;
    
    int result;
    
    pthread_mutex_lock(&ctx->status_lock);
    
    result = ctx->buffer[ctx->rd++];
    ctx->len--;
    
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
                    // fprintf(stderr, "%hhu\n", buf[i]);
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
    
    if (ctx->writing) {
        ctx->writing = 0;
        pthread_cancel(ctx->writer);
    }
    return NULL;
}

void *tty_writer(void *vctx) {
    ist66_tty_t *ctx = (ist66_tty_t *) vctx;
    ctx->writing = 1;
    
    while (ctx->running) {
        pthread_mutex_lock(&ctx->intr_lock);
        while (!ctx->command) {
            pthread_cond_wait(&ctx->write_cond, &ctx->intr_lock);
            
        }
        
        send(ctx->sock_console, &ctx->send, 1, 0);
        ctx->command = 0;
        pthread_mutex_unlock(&ctx->intr_lock);
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
            pthread_create
                (&ctx->writer, NULL, tty_writer, ctx);
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
    ist66_tty_t *ctx = (ist66_tty_t *) vctx;
    ist66_cu_t *cpu = ctx->cpu;
    
    if (transfer == 1) {
        ctx->send = (uint8_t) data;
    }
    
    else if (transfer == 3) {
        ctx->control = data >> 8;
        ctx->threshold = data & 0xFF;
    }
    
    if (transfer != 14) {
        switch (ctl) {
            case 1: {
                pthread_mutex_lock(&ctx->intr_lock);
                ctx->command = 1;
                if (ctx->done) {
                    ctx->done = 0;
                    intr_release(cpu, ctx->irq);
                }
                pthread_cond_signal(&ctx->write_cond);
                pthread_mutex_unlock(&ctx->intr_lock);
            } break;
            case 2: {
                pthread_mutex_lock(&ctx->intr_lock);
                ctx->command = 0;
                if (ctx->done) {
                    ctx->done = 0;
                    intr_release(cpu, ctx->irq);
                }
                pthread_mutex_unlock(&ctx->intr_lock);
            } break;
        }
    }
    
    if (transfer == 14) {
        int status = (ctx->done << 1) | (ctx->command & 1);
        return (uint64_t) status;
    }
    
    else if (transfer == 0) {
        return pop_char(ctx);
    }
    
    else return 0;
}

void destroy_tty(ist66_cu_t *cpu, int id) {
    ist66_tty_t *ctx = (ist66_tty_t *) cpu->ioctx[id];
    
    if (ctx->running) {
        pthread_cancel(ctx->reader);
        close(ctx->sock_console);
    }
    
    if (ctx->writing) {
        pthread_cancel(ctx->writer);
    }
    
    if (ctx->listening) {
        pthread_cancel(ctx->listener);
    }
    
    close(ctx->sock_listener);
    
    pthread_mutex_destroy(&ctx->status_lock);
    pthread_mutex_destroy(&ctx->intr_lock);
    pthread_cond_destroy(&ctx->write_cond);
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
    pthread_mutex_init(&ctx->intr_lock, NULL);
    pthread_cond_init(&ctx->write_cond, NULL);
    
    ctx->listening = 1;
    ctx->control = DEFAULTS;
    
    pthread_create(&ctx->listener, NULL, tty_listener, ctx);
    fprintf(stderr, "/DEV-I-UNIT %04o TTY IRQ %02o %d\n", id, irq, port);
}
