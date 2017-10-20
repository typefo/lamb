
/* 
 * Lamb Gateway Platform
 * Copyright (C) 2017 typefo <typefo@qq.com>
 * Update: 2017-07-14
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <nanomsg/nn.h>
#include <nanomsg/pair.h>
#include <nanomsg/reqrep.h>
#include "utils.h"
#include "config.h"
#include "cache.h"
#include "queue.h"
#include "socket.h"
#include "pool.h"
#include "md.h"

static lamb_cache_t rdb;
static lamb_queue_t *queue;
static lamb_config_t config;
static lamb_rep_t resp;
static pthread_cond_t cond;
static pthread_mutex_t mutex;

int main(int argc, char *argv[]) {
    char *file = "md.conf";
    bool background = false;

    int opt = 0;
    char *optstring = "c:d";
    opt = getopt(argc, argv, optstring);

    while (opt != -1) {
        switch (opt) {
        case 'c':
            file = optarg;
            break;
        case 'd':
            background = true;
            break;
        }
        opt = getopt(argc, argv, optstring);
    }

    /* Read lamb configuration file */
    if (lamb_read_config(&config, file) != 0) {
        return -1;
    }

    /* Daemon mode */
    if (background) {
        lamb_daemon();
    }

    /* Signal event processing */
    lamb_signal_processing();

    /* Start Main Event Thread */
    lamb_set_process("lamb-mdd");
    lamb_event_loop();

    return 0;
}

void lamb_event_loop(void) {
    int fd, err;

    pthread_cond_init(&cond, NULL);
    pthread_mutex_init(&mutex, NULL);
    
    /* Client Queue Pools Initialization */
    queue = lamb_queue_new(0);
    if (!queue) {
        lamb_errlog(config.logfile, "lamb queue initialization failed", 0);
        return;
    }

    /* Redis Cache Initialization */
    err = lamb_cache_connect(&rdb, config.redis_host, config.redis_port, NULL, config.redis_db);
    if (err) {
        lamb_errlog(config.logfile, "can't connect to redis %s server", config.redis_host);
        return;
    }

    /* MT Server Initialization */
    err = lamb_server_init(&fd, config.listen, config.port);
    if (err) {
        return;
    }

    /* Start Data Acquisition Thread */
    lamb_start_thread(lamb_stat_loop, NULL, 1);

    int rc, len;
    lamb_req_t *req;

    len = sizeof(lamb_req_t);
    
    while (true) {
        char *buf = NULL;
        rc = nn_recv(fd, &buf, NN_MSG, 0);

        if (rc != len) {
            nn_freemsg(buf);
            lamb_errlog(config.logfile, "Invalid request from client", 0);
            continue;
        }

        req = (lamb_req_t *)buf;

        if (req->id < 1) {
            nn_freemsg(buf);
            lamb_errlog(config.logfile, "Requests from unidentified clients", 0);
            continue;
        }

        struct timeval now;
        struct timespec timeout;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec + 5;

        pthread_mutex_lock(&mutex);
        memset(&resp, 0, sizeof(resp));
        lamb_start_thread(lamb_work_loop,  (void *)buf, 1);
        pthread_cond_timedwait(&cond, &mutex, &timeout);
        nn_send(fd, (char *)&resp, sizeof(resp), 0);
        pthread_mutex_unlock(&mutex);
    }
}

void *lamb_work_loop(void *arg) {
    int err;
    int fd, rc;
    lamb_node_t *node;
    lamb_req_t *client;
    
    client = (lamb_req_t *)arg;

    printf("-> new client from %s connectd\n", client->addr);

    /* Client channel initialization */
    unsigned short port = 30001;
    err = lamb_child_server(&fd, config.listen, &port);
    if (err) {
        pthread_cond_signal(&cond);
        lamb_errlog(config.logfile, "lamb can't find available port", 0);
        pthread_exit(NULL);
    }

    pthread_mutex_lock(&mutex);

    resp.id = client->id;
    memcpy(resp.addr, config.listen, 16);
    resp.port = port;

    pthread_mutex_unlock(&mutex);
    pthread_cond_signal(&cond);

    /* Start event processing */
    int len;

    len = sizeof(lamb_message_t);
    
    while (true) {
        char *buf = NULL;
        rc = nn_recv(fd, &buf, NN_MSG, NN_DONTWAIT);
        
        if (rc < 0) {
            if (err > 3) {
                break;
            }
            
            if (!nn_get_statistic(fd, NN_STAT_CURRENT_CONNECTIONS)) {
                err++;
                lamb_sleep(1000);
            }

            lamb_sleep(10);
            continue;
        }

        if (rc == len) {
            lamb_queue_push(queue, buf);
            continue;
        }

        if (rc >= 3) {
            if (strncasecmp(buf, "bye", 3) == 0) {
                nn_freemsg(buf);
                break;
            } else if (strncasecmp(buf, "req", 3) == 0) {
                node = lamb_queue_pop(queue);
                nn_send(fd, node ? node->val : "0", node ? len : 1, 0);
                if (node) {
                    free(node->val);
                    free(node);
                }
            }
            nn_freemsg(buf);
        }
    }

    nn_close(fd);
    nn_freemsg((char *)client);
    printf("-> connection closed from %s\n", client->addr);
    lamb_errlog(config.logfile, "connection closed from %s", client->addr);

    pthread_exit(NULL);
}

int lamb_server_init(int *sock, const char *listen, int port) {
    int fd;
    char addr[128];

    sprintf(addr, "tcp://%s:%d", listen, port);
    
    fd = nn_socket(AF_SP, NN_REP);
    if (fd < 0) {
        lamb_errlog(config.logfile, "socket %s", nn_strerror(nn_errno()));
        return -1;
    }

    if (nn_bind(fd, addr) < 0) {
        nn_close(fd);
        lamb_errlog(config.logfile, "bind %s", nn_strerror(nn_errno()));
        return -1;
    }

    *sock = fd;

    return 0;
}

int lamb_child_server(int *sock, const char *listen, unsigned short *port) {
    int fd;
    char addr[128];
    
    fd = nn_socket(AF_SP, NN_PAIR);
    if (fd < 0) {
        lamb_errlog(config.logfile, "socket %s", nn_strerror(nn_errno()));
        return -1;
    }

    while (true) {
        sprintf(addr, "tcp://%s:%u", listen, *port);

        if (nn_bind(fd, addr) > 0) {
            break;
        }

        (*port)++;
    }

    *sock = fd;
    
    return 0;
}

void *lamb_stat_loop(void *arg) {
    redisReply *reply;
    
    while (true) {
        reply = redisCommand(rdb.handle, "HMSET md.%d queue %lld", config.id, queue->len);
        if (reply != NULL) {
            freeReplyObject(reply);
        }
        printf("-> queue: %d, len: %lld\n", queue->id, queue->len);

        lamb_sleep(3000);
    }

    pthread_exit(NULL);
}

int lamb_read_config(lamb_config_t *conf, const char *file) {
    if (!conf) {
        return -1;
    }

    config_t cfg;
    if (lamb_read_file(&cfg, file) != 0) {
        fprintf(stderr, "ERROR: Can't open the %s configuration file\n", file);
        goto error;
    }

    if (lamb_get_int(&cfg, "Id", &conf->id) != 0) {
        fprintf(stderr, "ERROR: Can't read 'Id' parameter\n");
        goto error;
    }

    if (lamb_get_bool(&cfg, "Debug", &conf->debug) != 0) {
        fprintf(stderr, "ERROR: Can't read 'Debug' parameter\n");
        goto error;
    }

    if (lamb_get_string(&cfg, "Listen", conf->listen, 16) != 0) {
        fprintf(stderr, "ERROR: Invalid Listen IP address\n");
        goto error;
    }

    if (lamb_get_int(&cfg, "Port", &conf->port) != 0) {
        fprintf(stderr, "ERROR: Can't read 'Port' parameter\n");
        goto error;
    }

    if (lamb_get_int64(&cfg, "Timeout", &conf->timeout) != 0) {
        fprintf(stderr, "ERROR: Can't read 'Timeout' parameter\n");
        goto error;
    }

    if (lamb_get_string(&cfg, "RedisHost", conf->redis_host, 16) != 0) {
        fprintf(stderr, "ERROR: Can't read 'RedisHost' parameter\n");
    }

    if (lamb_get_int(&cfg, "RedisPort", &conf->redis_port) != 0) {
        fprintf(stderr, "ERROR: Can't read 'RedisPort' parameter\n");
    }

    if (conf->redis_port < 1 || conf->redis_port > 65535) {
        fprintf(stderr, "ERROR: Invalid redis port number\n");
        goto error;
    }

    if (lamb_get_int(&cfg, "RedisDb", &conf->redis_db) != 0) {
        fprintf(stderr, "ERROR: Can't read 'RedisDb' parameter\n");
    }

    if (lamb_get_string(&cfg, "LogFile", conf->logfile, 128) != 0) {
        fprintf(stderr, "ERROR: Can't read 'LogFile' parameter\n");
        goto error;
    }

    lamb_config_destroy(&cfg);
    return 0;
error:
    lamb_config_destroy(&cfg);
    return -1;
}

