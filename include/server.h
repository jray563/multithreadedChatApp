#ifndef SERVER_H
#define SERVER_H

#include <arpa/inet.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFFER_SIZE 1024
#define SA struct sockaddr

typedef struct user user;
typedef struct room room;
typedef struct job job;
typedef struct jobQueue jobQueue;
typedef struct roomList roomList;
typedef struct auditLog auditLog;

void run_server(int server_port);

struct user {
    char *username;
    int fd;
    user *next;
};

struct room {
    char *roomName;
    user* creator;
    user* userList;
    room *next;
};

struct roomList {
    room *head;
    pthread_mutex_t roomListMutex;
};

struct job {
    char *msg;
    user *client;
    job *next;
    job *prev;
};

struct jobQueue {
    job *head, *tail;
    size_t size;
    pthread_mutex_t jobQueueMutex;
    pthread_cond_t notEmpty;
};

 struct auditLog {
    char *fileName;
    pthread_mutex_t auditLogMutex;
};

#endif
