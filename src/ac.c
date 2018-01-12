
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
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <nanomsg/nn.h>
#include <nanomsg/pair.h>
#include <nanomsg/reqrep.h>
#include <errno.h>
#include <cmpp.h>
#include "ac.h"
#include "utils.h"
#include "cache.h"
#include "list.h"
#include "queue.h"
#include "socket.h"
#include "config.h"

static lamb_db_t db;
static lamb_cache_t rdb;
static lamb_config_t config;
static lamb_list_t *mt;
static lamb_list_t *mo;
static lamb_list_t *ismg;
static lamb_list_t *server;
static lamb_list_t *scheduler;
static lamb_list_t *delivery;
static lamb_list_t *gateway;

int main(int argc, char *argv[]) {
    char *file = "ac.conf";
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

    /* log file setting */
    if (setenv("logfile", config->logfile, 1) == -1) {
        lamb_vlog(LOG_ERR, "setenv error: %s", strerror(errno));
        return -1;
    }
    
    /* Signal event processing */
    lamb_signal_processing();

    /* Start Main Event Thread */
    lamb_event_loop();

    return 0;
}

void lamb_event_loop(void) {
    int fd;
    int err;
    void *pk;
    int rc, len;
    Request *req;
    char *buf = NULL;
    unsigned short port;
    Response resp = LAMB_RESPONSE_INIT;

    lamb_set_process("lamb-acd");

    pthread_cond_init(&cond, NULL);
    pthread_mutex_init(&mutex, NULL);

    err = lamb_component_initialization(&config);

    if (err) {
        return;
    }

    /* Start mt processing thread */
    lamb_start_thread(lamb_mt_loop, NULL, 1);

    /* Start mo processing thread */
    lamb_start_thread(lamb_mo_loop, NULL, 1);
    
    /* Start ismg processing thread */
    lamb_start_thread(lamb_ismg_loop, NULL, 1);
    
    /* Start server processing thread */
    lamb_start_thread(lamb_server_loop, NULL, 1);
    
    /* Start scheduler processing thread */
    lamb_start_thread(lamb_scheduler_loop, NULL, 1);
    
    /* Start delivery processing thread */
    lamb_start_thread(lamb_delivery_loop, NULL, 1);
    
    /* Start gateway processing thread */
    lamb_start_thread(lamb_gateway_loop, NULL, 1);

    /* Start main loop thread */
    err = lamb_nn_server(&fd, config.listen, config.port, NN_REP);
    if (err) {
        lamb_log(LOG_ERR, "ac server initialization failed");
        return;
    }

    while (true) {
        rc = nn_recv(fd, &buf, NN_MSG, 0);

        if (rc < HEAD) {
            if (rc > 0) {
                nn_freemsg(buf);
            }
            continue;
        }

        if (CHECK_COMMAND(buf) != LAMB_REQUEST) {
            nn_freemsg(buf);
            lamb_log(LOG_WARNING, "invalid request from client");
            continue;
        }

        req = lamb_request_unpack(NULL, rc - HEAD, (uint8_t *)(buf + HEAD));
        nn_freemsg(buf);

        if (!req) {
            lamb_log(LOG_ERR, "can't unpack request protocol packets");
            continue;
        }

        if (req->id < 1) {
            lamb_request_free_unpacked(req, NULL);
            lamb_log(LOG_WARNING, "can't recognition client identity");
            continue;
        }

        int cli;
        port = config.port + 1;
        err = lamb_child_server(&cli, config.listen, &port, NN_PAIR);

        if (err) {
            lamb_request_free_unpacked(req, NULL);
            lamb_log(LOG_ERR, "lamb can't find available port");

            continue;
        }

        resp.id = req->id;
        resp.addr = config.listen;
        resp.port = port;

        timeout = config.timeout;
        nn_setsockopt(fd, NN_SOL_SOCKET, NN_RCVTIMEO, &timeout, sizeof(timeout));

        len = lamb_response_get_packed_size(&resp);
        pk = malloc(len);

        if (!pk) {
            lamb_request_free_unpacked(req, NULL);
            continue;
        }

        lamb_response_pack(&resp, pk);
        len = lamb_pack_assembly(&buf, LAMB_RESPONSE, pk, len);
        if (len > 0) {
            nn_send(fd, buf, len, 0);
            free(buf);
        }

        free(pk);

        if (req->type == LAMB_MT) {
            lamb_list_rpush(mt, req);
        } else if (req->type == LAMB_MO) {
            lamb_list_rpush(mt, req);
        } else if (req->type == LAMB_ISMG) {
            lamb_list_rpush(mt, req);
        } else if (req->type == LAMB_SERVER) {
            lamb_list_rpush(mt, req);
        } else if (req->type == LAMB_SCHEDULER) {
            lamb_list_rpush(mt, req);
        } else if (req->type == LAMB_DELIVERY) {
            lamb_list_rpush(mt, req);
        } else if (req->type == LAMB_GATEWAY) {
            lamb_list_rpush(mt, req);
        } else {
            nn_close(cli);
            lamb_log(LOG_ERR, "invalid type of client");
        }

        lamb_request_free_unpacked(req, NULL);
    }

    return;
}

void *lamb_mt_loop(void *arg) {
    int fd, err;
    int timeout;
}

void *lamb_mo_loop(void *arg) {

}

void *lamb_ismg_loop(void *arg) {

}

void *lamb_server_loop(void *arg) {
    
}

void *lamb_scheduler_loop(void *arg) {

}

void *lamb_delivery_loop(void *arg) {

}

void *lamb_gateway_loop(void *arg) {

}

int lamb_component_initialization(lamb_config_t *cfg) {
    if (!cfg) {
        return -1;
    }

    /* mt object queue initialization */
    mt = lamb_list_new();
    if (!mt) {
        lamb_log(LOG_ERR, "mt object queue initialization failed");
        return -1;
    }

    /* mo object queue initialization */
    mo = lamb_list_new();
    if (!mo) {
        lamb_log(LOG_ERR, "mo object queue initialization failed");
        return -1;
    }

    /* ismg object queue initialization */
    ismg = lamb_list_new();
    if (!ismg) {
        lamb_log(LOG_ERR, "ismg object queue initialization failed");
        return -1;
    }

    /* server object queue initialization */
    server = lamb_list_new();
    if (!server) {
        lamb_log(LOG_ERR, "server object queue initialization failed");
        return -1;
    }

    /* scheduler object queue initialization */
    scheduler = lamb_list_new();
    if (!scheduler) {
        lamb_log(LOG_ERR, "scheduler object queue initialization failed");
        return -1;
    }

    /* delivery object queue initialization */
    delivery = lamb_list_new();
    if (!delivery) {
        lamb_log(LOG_ERR, "delivery object queue initialization failed");
        return -1;
    }

    /* gateway object queue initialization */
    gateway = lamb_list_new();
    if (!gateway) {
        lamb_log(LOG_ERR, "gateway object queue initialization failed");
        return -1;
    }

    /* redis initialization */
    err = lamb_cache_connect(&rdb, cfg->redis_host, cfg->redis_port, NULL,
                             cfg->redis_db);
    if (err) {
        lamb_log(LOG_ERR, "can't connect to redis server %s", cfg->redis_host);
        return -1;
    }

    /* database initialization */
    err = lamb_db_init(&db);
    if (err) {
        lamb_log(LOG_ERR, "database initialization failed");
        return -1;
    }

    err = lamb_db_connect(&db, cfg->db_host, cfg->db_port, cfg->db_user,
                          cfg->db_password, cfg->db_name);
    if (err) {
        lamb_log(LOG_ERR, "can't connect to database %s", cfg->db_host);
        return -1;
    }

    return 0;
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

    if (lamb_get_bool(&cfg, "Daemon", &conf->daemon) != 0) {
        fprintf(stderr, "ERROR: Can't read 'Daemon' parameter\n");
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

    if (conf->port < 1 || conf->port > 65535) {
        fprintf(stderr, "ERROR: Invalid listen port number\n");
        goto error;
    }

    if (lamb_get_int(&cfg, "Connections", &conf->connections) != 0) {
        fprintf(stderr, "ERROR: Can't read 'Connections' parameter\n");
        goto error;
    }

    if (lamb_get_int64(&cfg, "Timeout", &conf->timeout) != 0) {
        fprintf(stderr, "ERROR: Can't read 'Timeout' parameter\n");
        goto error;
    }

    if (lamb_get_int64(&cfg, "RecvTimeout", &conf->recv_timeout) != 0) {
        fprintf(stderr, "ERROR: Can't read 'RecvTimeout' parameter\n");
        goto error;
    }

    if (lamb_get_int64(&cfg, "SendTimeout", &conf->send_timeout) != 0) {
        fprintf(stderr, "ERROR: Can't read 'SendTimeout' parameter\n");
        goto error;
    }

    if (lamb_get_string(&cfg, "redisHost", conf->redis_host, 16) != 0) {
        fprintf(stderr, "ERROR: Can't read 'redisHost' parameter\n");
    }

    if (lamb_get_int(&cfg, "redisPort", &conf->redis_port) != 0) {
        fprintf(stderr, "ERROR: Can't read 'redisPort' parameter\n");
    }

    if (conf->redis_port < 1 || conf->redis_port > 65535) {
        fprintf(stderr, "ERROR: Invalid redis port number\n");
        goto error;
    }

    if (lamb_get_int(&cfg, "redisDb", &conf->redis_db) != 0) {
        fprintf(stderr, "ERROR: Can't read 'redisDb' parameter\n");
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

