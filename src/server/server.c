#include "server.h"
#include "protocol.h"
#define __USE_GNU
#include <pthread.h>
#include <signal.h>

pthread_mutexattr_t attr;

struct {
    pthread_mutex_t usersMutex;
    struct user *userList;
} users;

jobQueue jobs;
roomList rooms;
auditLog aLog;

time_t t;

const char exit_str[] = "exit";

char buffer[BUFFER_SIZE];
pthread_mutex_t buffer_lock;

int total_num_msg = 0;
int listen_fd;

void sigint_handler(int sig) {
    printf("shutting down server\n");
    close(listen_fd);
    exit(0);
}

static void serverSendAudit(int msg_type, user *u) {
     pthread_mutex_lock(&aLog.auditLogMutex);

    FILE *file = fopen(aLog.fileName, "a");
    time(&t);
    fprintf(file, "Server sent: %x to %s [%d] at %s\n", msg_type, u->username, u->fd, ctime(&t));
    fclose(file);

    pthread_mutex_unlock(&aLog.auditLogMutex);
}

int server_init(int server_port) {
    int sockfd;
    struct sockaddr_in servaddr;

    // socket create and verification
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        exit(EXIT_FAILURE);
    } else
        printf("Socket successfully created\n");

    bzero(&servaddr, sizeof(servaddr));

    // assign IP, PORT
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(server_port);

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, (char *)&opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // Binding newly created socket to given IP and verification
    if ((bind(sockfd, (SA *)&servaddr, sizeof(servaddr))) != 0) {
        printf("socket bind failed\n");
        exit(EXIT_FAILURE);
    } else
        printf("Socket successfully binded\n");

    // Now server is ready to listen and verification
    if ((listen(sockfd, 1)) != 0) {
        printf("Listen failed\n");
        exit(EXIT_FAILURE);
    } else
        printf("Server listening on port: %d.. Waiting for connection\n", server_port);

    return sockfd;
}

static void getMsgAsStr(char *msg, char **str) {
    petr_header *header = (petr_header*)msg;
    *str = malloc(header->msg_len);
    memcpy(*str, (char*)header+sizeof(petr_header), header->msg_len);
}

void *process_job(void* arg) {

    while (1) {
        //printf("Made b");
        pthread_mutex_lock(&jobs.jobQueueMutex);

        while (jobs.size <= 0)
            pthread_cond_wait(&jobs.notEmpty, &jobs.jobQueueMutex);

        char *msg = jobs.tail->msg;
        petr_header *header = (petr_header*)msg;
        user *client = jobs.tail->client;

        switch (header->msg_type)
        {
        case RMCREATE:
            {
                pthread_mutex_lock(&rooms.roomListMutex);

                petr_header response;
                memset(&response, 0, sizeof(response));
                char *roomname;

                getMsgAsStr(msg, &roomname);

                room *temp = rooms.head;
                while (temp != NULL) {
                    if (strcmp(temp->roomName, roomname) == 0) {
                        response.msg_type = ERMEXISTS;
                        response.msg_len = 0;
                        if (wr_msg(client->fd, &response, NULL) < 0) {
                            printf("Write error\n");
                            exit(EXIT_FAILURE);
                        }
                        printf("Roomname already exists.\n");

                        serverSendAudit(response.msg_type, client);
                       
                        pthread_mutex_unlock(&rooms.roomListMutex);
                        goto finish;                    
                    }

                    temp = temp->next;
                }
                
                response.msg_type = OK;
                response.msg_len = 0;
                if (wr_msg(client->fd, &response, NULL) < 0) {
                    printf("Write error\n");
                    exit(EXIT_FAILURE);
                }

                serverSendAudit(response.msg_type, client);


                room *newRoom = malloc(sizeof(room));
                newRoom->roomName = roomname;

                user *newClient = malloc(sizeof(user));
                memcpy(newClient, client, sizeof(user));
                newClient->next = NULL;
                newRoom->creator = newRoom->userList = newClient;

                if (rooms.head == NULL) 
                    newRoom->next = NULL;
                else
                    newRoom->next = rooms.head;
                
                rooms.head = newRoom;

                pthread_mutex_unlock(&rooms.roomListMutex);
                printf("Room (%s) created.\n", roomname);
            }
            break;
        case RMDELETE:
            {
                pthread_mutex_lock(&rooms.roomListMutex);

                petr_header response;
                memset(&response, 0, sizeof(response));
                char *roomname;

                getMsgAsStr(msg, &roomname);

                room *temp = rooms.head;
                room *prev = NULL;
                while (temp != NULL) {
                    if (strcmp(temp->roomName, roomname) == 0) {
                        if (strcmp(client->username, temp->creator->username) == 0) {
                            // RMCLOSED
                            response.msg_type = RMCLOSED;
                            response.msg_len = strlen(roomname)+1;

                            user *temp2 = temp->userList;
                            while (temp2->next != NULL) {
                                if (wr_msg(temp2->fd, &response, roomname) < 0) {
                                    printf("Write error\n");
                                    exit(EXIT_FAILURE);
                                }
                                serverSendAudit(response.msg_type, temp2);
                                temp2 = temp2->next;
                            }
                            
                            response.msg_type = OK;
                            response.msg_len = 0;
                            if (wr_msg(client->fd, &response, NULL) < 0) {
                                printf("Write error\n");
                                exit(EXIT_FAILURE);
                            }
                            serverSendAudit(response.msg_type, client);

                            temp2 = temp->userList;
                            while (temp2 != NULL) {
                                user *temp3 = temp2;
                                temp2 = temp2->next;
                                free(temp3);
                            }

                            if (prev == NULL)
                                rooms.head = temp->next;
                            else
                                prev->next = temp->next;
                            
                            free(temp->roomName);
                            free(temp);

                            printf("Room (%s) closed.\n", roomname);
                            pthread_mutex_unlock(&rooms.roomListMutex);
                            goto finish;
                        } else {
                            // ERMDENIED
                            response.msg_type = ERMDENIED;
                            response.msg_len = 0;
                            if (wr_msg(client->fd, &response, NULL) < 0) {
                                printf("Write error\n");
                                exit(EXIT_FAILURE);
                            }
                            printf("User is not creator of room\n");
                            serverSendAudit(response.msg_type, client);
                            pthread_mutex_unlock(&rooms.roomListMutex);
                            goto finish;
                        }                 
                    }
                    prev = temp;
                    temp = temp->next;
                }

                // ERMNOTFOUND
                response.msg_type = ERMNOTFOUND;
                response.msg_len = 0;
                if (wr_msg(client->fd, &response, NULL) < 0) {
                    printf("Write error\n");
                    exit(EXIT_FAILURE);
                }
                
                printf("Roomname (%s) not found.\n", roomname);
                serverSendAudit(response.msg_type, client);

                pthread_mutex_unlock(&rooms.roomListMutex);
            }
            break;
        case RMLIST:
            {
                pthread_mutex_lock(&rooms.roomListMutex);

                petr_header response;
                memset(&response, 0, sizeof(response));
                response.msg_type = RMLIST;

                if (rooms.head == NULL) {
                    response.msg_len = 0;
                    if (wr_msg(client->fd, &response, NULL) < 0) {
                        printf("Write error\n");
                        exit(EXIT_FAILURE);
                    }
                    serverSendAudit(response.msg_type, client);
                    pthread_mutex_unlock(&rooms.roomListMutex);
                    goto finish;
                }
                
                pthread_mutex_lock(&buffer_lock);
                bzero(buffer, BUFFER_SIZE);

                room *temp = rooms.head;
                int offset= 0;
                while (temp != NULL){
                    offset += snprintf(buffer+offset, strlen(temp->roomName)+3, "%s: ", temp->roomName);
                    user* curUser = temp->userList;
                    while(curUser != NULL) {
                        offset += snprintf(buffer+offset, strlen(curUser->username)+2, "%s,", curUser->username);
                        curUser = curUser->next;
                    }
                    buffer[offset-1] = '\n';
                    temp = temp->next;
                }    
                buffer[offset] = '\0';

                response.msg_len = offset+1;
                if (wr_msg(client->fd, &response, buffer) < 0) {
                        printf("Write error\n");
                        exit(EXIT_FAILURE);
                }
                serverSendAudit(response.msg_type, client);
                pthread_mutex_unlock(&buffer_lock);
                pthread_mutex_unlock(&rooms.roomListMutex);
            }
            break;
        case RMJOIN:
            {
                pthread_mutex_lock(&rooms.roomListMutex);

                petr_header response;
                memset(&response, 0, sizeof(response));
                char *roomname;

                getMsgAsStr(msg, &roomname);

                room *temp = rooms.head;
                while (temp != NULL) {
                    if (strcmp(temp->roomName, roomname) == 0) {
                        user *newClient = malloc(sizeof(user));
                        memcpy(newClient, client, sizeof(user));
                        newClient->next = temp->userList;
                        temp->userList = newClient;

                        response.msg_type = OK;
                        response.msg_len = 0;
                        if (wr_msg(client->fd, &response, NULL) < 0) {
                            printf("Write error\n");
                            exit(EXIT_FAILURE);
                        }

                        printf("Room (%s) joined.\n", roomname);
                        serverSendAudit(response.msg_type, client);
                        pthread_mutex_unlock(&rooms.roomListMutex);
                        goto finish;
                    }
                    temp = temp->next;
                }

                // ERMNOTFOUND
                response.msg_type = ERMNOTFOUND;
                response.msg_len = 0;
                if (wr_msg(client->fd, &response, NULL) < 0) {
                    printf("Write error\n");
                    exit(EXIT_FAILURE);
                }
                
                printf("Roomname (%s) not found.\n", roomname);
                serverSendAudit(response.msg_type, client);
                pthread_mutex_unlock(&rooms.roomListMutex);
            }
            break;
        case RMLEAVE:
            {
                pthread_mutex_lock(&rooms.roomListMutex);

                petr_header response;
                memset(&response, 0, sizeof(response));
                char *roomname;

                getMsgAsStr(msg, &roomname);

                room *temp = rooms.head;
                while (temp != NULL) {
                    if (strcmp(temp->roomName, roomname) == 0) {
                        user *prev = NULL;
                        user *temp2 = temp->userList;
                        while (temp2 != NULL) {
                            if (strcmp(temp2->username, client->username) == 0) {
                                if (strcmp(client->username, temp->creator->username) == 0) {
                                    response.msg_type = ERMDENIED;
                                    response.msg_len = 0;
                                    if (wr_msg(client->fd, &response, NULL) < 0) {
                                        printf("Write error\n");
                                        exit(EXIT_FAILURE);
                                    }
                                    serverSendAudit(response.msg_type, client);
                                    pthread_mutex_unlock(&rooms.roomListMutex);
                                    goto finish;
                                } else {
                                    if (prev == NULL)
                                        temp->userList = temp2->next;
                                    else
                                        prev->next = temp2->next;

                                    free(temp2);

                                    response.msg_type = OK;
                                    response.msg_len = 0;
                                    if (wr_msg(client->fd, &response, NULL) < 0) {
                                        printf("Write error\n");
                                        exit(EXIT_FAILURE);
                                    }
                                    serverSendAudit(response.msg_type, client);
                                    pthread_mutex_unlock(&rooms.roomListMutex);
                                    goto finish;
                                }
                            }
                            prev = temp2;
                            temp2 = temp2->next;
                        }
                        
                        response.msg_type = OK;
                        response.msg_len = 0;
                        if (wr_msg(client->fd, &response, NULL) < 0) {
                            printf("Write error\n");
                            exit(EXIT_FAILURE);
                        }
                        serverSendAudit(response.msg_type, client);

                        pthread_mutex_unlock(&rooms.roomListMutex);
                        goto finish;
                    }
                    temp = temp->next;
                }

                // ERMNOTFOUND
                response.msg_type = ERMNOTFOUND;
                response.msg_len = 0;
                if (wr_msg(client->fd, &response, NULL) < 0) {
                    printf("Write error\n");
                    exit(EXIT_FAILURE);
                }
                
                printf("Roomname (%s) not found.\n", roomname);
                serverSendAudit(response.msg_type, client);

                pthread_mutex_unlock(&rooms.roomListMutex);
            }
            break;
        case RMSEND:
            {
                pthread_mutex_lock(&rooms.roomListMutex);
                petr_header response;
                memset(&response, 0, sizeof(response));
                
                char *msgToSend;
                char *save_ptr;
                getMsgAsStr(msg, &msgToSend);
                char *roomname = strtok_r(msgToSend, "\r\n", &save_ptr);
                msgToSend = save_ptr + 1;

                room *temp = rooms.head;
                while (temp != NULL) {
                    if (strcmp(temp->roomName, roomname) == 0) {
                        user *temp2 = temp->userList;
                        while (temp2 != NULL) {
                            if (strcmp(temp2->username, client->username) == 0) {
                                user* temp3 = temp->userList;
                                while (temp3 != NULL) {
                                    if (temp3 != temp2) {
                                        char *message = malloc(strlen(roomname)+strlen(client->username)+strlen(msgToSend)+4+1);
                                        strcpy(message, roomname);
                                        strcat(message, "\r\n");
                                        strcat(message, client->username);
                                        strcat(message, "\r\n");
                                        strcat(message, msgToSend);

                                        response.msg_type = RMRECV;
                                        response.msg_len = strlen(message)+1;
                                        
                                        if (wr_msg(temp3->fd, &response, message) < 0) {
                                            printf("Write error\n");
                                            exit(EXIT_FAILURE);
                                        }
                                        serverSendAudit(response.msg_type, temp3);

                                    }
                                    temp3 = temp3->next;
                                }

                                response.msg_type = OK;
                                response.msg_len = 0;

                                if (wr_msg(client->fd, &response, NULL) < 0) {
                                    printf("Write error\n");
                                    exit(EXIT_FAILURE);
                                }
                                serverSendAudit(response.msg_type, client);

                                pthread_mutex_unlock(&rooms.roomListMutex);
                                goto finish;
                            }
                            temp2 = temp2->next;
                        }

                        response.msg_type = ERMDENIED;
                        response.msg_len = 0;
                        if (wr_msg(client->fd, &response, NULL) < 0) {
                            printf("Write error\n");
                            exit(EXIT_FAILURE);
                        }
                        serverSendAudit(response.msg_type, client);

                        pthread_mutex_unlock(&rooms.roomListMutex);
                        goto finish;
                    }
                    temp = temp->next;
                }

                // ERMNOTFOUND
                response.msg_type = ERMNOTFOUND;
                response.msg_len = 0;
                if (wr_msg(client->fd, &response, NULL) < 0) {
                    printf("Write error\n");
                    exit(EXIT_FAILURE);
                }
                serverSendAudit(response.msg_type, client);
                
                printf("Roomname (%s) not found.\n", roomname);

                pthread_mutex_unlock(&rooms.roomListMutex);
            }
            break;
        case USRSEND:
            {
                pthread_mutex_lock(&users.usersMutex);
                petr_header response;
                memset(&response, 0, sizeof(response));

                char *msgToSend;
                char *save_ptr;
                getMsgAsStr(msg, &msgToSend);
                char *to_username = strtok_r(msgToSend, "\r\n", &save_ptr);
                msgToSend = save_ptr + 1;

                user* temp2 = users.userList;
                while (temp2 != NULL){
                    if (strcmp(temp2->username, to_username) == 0){
                        char *message = malloc(strlen(client->username)+strlen(msgToSend)+2+1);
                        strcpy(message, client->username);
                        strcat(message, "\r\n");
                        strcat(message, msgToSend);
                      

                        response.msg_type = USRRECV;
                        response.msg_len = strlen(message)+1;
                        
                        if (wr_msg(temp2->fd, &response, message) < 0) {
                            printf("Write error\n");
                            exit(EXIT_FAILURE);
                        }
                        serverSendAudit(response.msg_type, temp2);

                        response.msg_type = OK;
                        response.msg_len = 0;

                        if (wr_msg(client->fd, &response, NULL) < 0) {
                            printf("Write error\n");
                            exit(EXIT_FAILURE);
                        }
                        serverSendAudit(response.msg_type, client);
                        
                        pthread_mutex_unlock(&users.usersMutex);
                        goto finish;
                    }
                    temp2 = temp2->next;
                }
                
                //EUSRNOTFOUND
                response.msg_type = EUSRNOTFOUND;
                response.msg_len = 0;
                if (wr_msg(client->fd, &response, NULL) < 0) {
                    printf("Write error\n");
                    exit(EXIT_FAILURE);
                }
                serverSendAudit(response.msg_type, client);
                printf("User (%s) not found.\n", to_username);
                pthread_mutex_unlock(&users.usersMutex);
            }
            break;
        case USRLIST:
            {
                pthread_mutex_lock(&users.usersMutex);

                petr_header response;
                memset(&response, 0, sizeof(response));
                response.msg_type = USRLIST;

                if (users.userList == NULL) {
                    response.msg_len = 0;
                    if (wr_msg(client->fd, &response, NULL) < 0) {
                        printf("Write error\n");
                        exit(EXIT_FAILURE);
                    }
                    serverSendAudit(response.msg_type, client);
                
                    pthread_mutex_unlock(&users.usersMutex);
                    goto finish;
                }
                
                pthread_mutex_lock(&buffer_lock);
                bzero(buffer, BUFFER_SIZE);

                user *temp = users.userList;
                int offset= 0;
                while (temp != NULL){
                    if (strcmp(temp->username, client->username) != 0) {
                        offset += snprintf(buffer+offset, strlen(temp->username)+2, "%s\n", temp->username);
                    }
                    temp = temp->next;
                }

                buffer[offset] = '\0';

                response.msg_len = !offset ? 0 : offset+1;
                if (wr_msg(client->fd, &response, buffer) < 0) {
                        printf("Write error\n");
                        exit(EXIT_FAILURE);
                }
                serverSendAudit(response.msg_type, client);

                pthread_mutex_unlock(&buffer_lock);
                pthread_mutex_unlock(&users.usersMutex);
            }
            break;
        case LOGOUT:
            {
                pthread_mutex_lock(&rooms.roomListMutex);
                petr_header response;
                memset(&response, 0, sizeof(response));

                room *prev = NULL;
                room *temp = rooms.head;
                while (temp != NULL) {
                    if (strcmp(client->username, temp->creator->username) == 0) {
                        // send rmclose
                        response.msg_type = RMCLOSED;
                        response.msg_len = strlen(temp->roomName)+1;

                        user *temp2 = temp->userList;
                        while (temp2->next != NULL) {
                            if (wr_msg(temp2->fd, &response, temp->roomName) < 0) {
                                printf("Write error\n");
                                exit(EXIT_FAILURE);
                            }
                            serverSendAudit(response.msg_type, temp2);
                            temp2 = temp2->next;
                        }
                        //free out room
                        if (prev == NULL)
                            rooms.head = temp->next;
                        else
                            prev->next = temp->next;

                        room *temp3 = temp->next;
                            
                        free(temp->roomName);
                        free(temp);

                        temp = temp3;
                        continue;
                    } else {
                        user *temp2 = temp->userList;
                        user *prev2 = NULL;
                        while (temp2 != NULL) {
                            if (strcmp(temp2->username, client->username) == 0) {
                                    if (prev2 == NULL)
                                        temp->userList = temp2->next;
                                    else
                                        prev2->next = temp2->next;
                                                                        
                                    free(temp2);
                                    break;
                            }
                        
                            prev2 = temp2;
                            temp2 = temp2->next;
                        }
                    }

                    prev = temp;
                    temp = temp->next;
                }
                pthread_mutex_unlock(&rooms.roomListMutex);
                pthread_mutex_lock(&users.usersMutex);

                user *curUser = users.userList;
                user *prev3 = NULL;
                while (curUser != NULL) {
                    if (strcmp(curUser->username, client->username) == 0) {
                        if (prev3 == NULL)
                            users.userList = curUser->next;
                        else
                            prev3->next = curUser->next;

                        response.msg_type = OK;
                        response.msg_len = 0;
                        if (wr_msg(client->fd, &response, NULL) < 0) {
                            printf("Write error\n");
                            exit(EXIT_FAILURE);
                        }
                        serverSendAudit(response.msg_type, client);
                        free(curUser->username);
                        free(curUser);
                        pthread_mutex_unlock(&users.usersMutex);
                        goto finish;
                    }
                    prev3 = curUser;
                    curUser = curUser->next;
                }
                pthread_mutex_unlock(&users.usersMutex);  
            }
            break;
        default:
            break;
        }

    finish:
        if (jobs.tail->prev == NULL) {
            free(jobs.tail);
            jobs.head = jobs.tail = NULL;
        } else {
            jobs.tail->prev->next = NULL;
            free(jobs.tail);
        }

        jobs.size--;

        pthread_mutex_lock(&aLog.auditLogMutex);

        FILE *file = fopen(aLog.fileName, "a");
        time(&t);
        fprintf(file, "Job thread [%ld] removed job at %s\n", (long)pthread_self(), ctime(&t));
        fclose(file);

        pthread_mutex_unlock(&aLog.auditLogMutex);

        pthread_mutex_unlock(&jobs.jobQueueMutex);
    }
    return NULL;
}

//Function running in thread
void *process_client(void *user_ptr) {
    user *client = (user *)user_ptr;
    int client_fd = client->fd;
    int received_size;
    fd_set read_fds;

    int retval;
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        retval = select(client_fd + 1, &read_fds, NULL, NULL, NULL);
        if (retval != 1 && !FD_ISSET(client_fd, &read_fds)) {
            printf("Error with select() function\n");
            break;
        }

        pthread_mutex_lock(&buffer_lock);

        bzero(buffer, BUFFER_SIZE);
        received_size = read(client_fd, buffer, sizeof(buffer));
        if (received_size < 0) {
            printf("Receiving failed\n");
            break;
        } else if (received_size == 0) {
            pthread_mutex_unlock(&buffer_lock);
            break;
            //continue;
        }


        petr_header *header = (petr_header*)buffer;
        pthread_mutex_lock(&aLog.auditLogMutex);

        FILE *file = fopen(aLog.fileName, "a");
        time(&t);
        fprintf(file, "Client sent: %x, %d %s at %s\n", header->msg_type, header->msg_len, ((char*)header+sizeof(petr_header)), ctime(&t));
        fclose(file);

        pthread_mutex_unlock(&aLog.auditLogMutex);
        pthread_mutex_lock(&jobs.jobQueueMutex);

        job* newJob = malloc(sizeof(job));
        newJob->msg = malloc(received_size);
        newJob->client = client;
        memcpy(newJob->msg, buffer, received_size);

        newJob->prev = NULL;
        if (jobs.head == NULL) {
            newJob->next = NULL;
            jobs.head = jobs.tail = newJob;
        } else {
            newJob->next = jobs.head;
            jobs.head->prev = newJob;
            jobs.head = newJob;
        }

        jobs.size++;

        pthread_mutex_lock(&aLog.auditLogMutex);

        file = fopen(aLog.fileName, "a");
        time(&t);
        fprintf(file, "Job thread [%ld] inserted job at %s\n", (long)pthread_self(), ctime(&t));
        fclose(file);

        pthread_mutex_unlock(&aLog.auditLogMutex);
     
        pthread_cond_signal(&jobs.notEmpty);
        pthread_mutex_unlock(&jobs.jobQueueMutex);

        pthread_mutex_unlock(&buffer_lock);

    }
    // Close the socket at the end
    printf("Close current client connection\n");
    close(client_fd);

    pthread_mutex_lock(&aLog.auditLogMutex);

    FILE *file = fopen(aLog.fileName, "a");
    time(&t);
    fprintf(file, "Client thread [%ld] terminated at %s\n", (long) pthread_self(), ctime(&t));
    fclose(file);

    pthread_mutex_unlock(&aLog.auditLogMutex);


    return NULL;
}

void run_server(int server_port) {
    listen_fd = server_init(server_port); // Initiate server and start listening on specified port
    int client_fd;
    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);
    int received_size;

    pthread_t tid;

    while (1) {
        begin:
        // Wait and Accept the connection from client
        printf("Wait for new client connection\n");
        int *client_fd = malloc(sizeof(int));
        *client_fd = accept(listen_fd, (SA *)&client_addr, (socklen_t*)&client_addr_len);
        if (*client_fd < 0) {
            printf("server acccept failed\n");
            exit(EXIT_FAILURE);
        } else {
            // TODO: Verify User name (reject and close on failed)
            //      -> add to user list -> spawn client thread
            
            pthread_mutex_lock(&buffer_lock);

            bzero(buffer, BUFFER_SIZE);
            received_size = read(*client_fd, buffer, sizeof(buffer));
            
            if (received_size < 0) {
                printf("Receiving failed\n");
                break;                
            } else if (received_size == 0) {
                printf("Nothing sent\n");
                continue;
            }
            
            petr_header *header = (petr_header*)buffer;
            if (header->msg_type != LOGIN || header->msg_len <= 0) {
                // TODO: send ESERV
                petr_header eservHeader;
                memset(&eservHeader, 0, sizeof(eservHeader));
                eservHeader.msg_type = ESERV;
                eservHeader.msg_len = 0;
                if (wr_msg(*client_fd, &eservHeader, NULL) < 0) {
                    printf("Write error\n");
                    exit(EXIT_FAILURE);
                }

                pthread_mutex_unlock(&buffer_lock);
                continue;
            }
            // char *username = malloc(header->msg_len+1);
            // memcpy(username, (char*)header + sizeof(petr_header), header->msg_len);
            // username[header->msg_len] = '\0';
            char *username;
            getMsgAsStr(buffer, &username);
            pthread_mutex_lock(&users.usersMutex); 
            struct user *temp = users.userList;
            bzero(buffer, BUFFER_SIZE);
            int ret;
            
            while (temp != NULL) {
                if (strcmp(username, temp->username) == 0) {
                    petr_header newHeader;
                    memset(&newHeader, 0, sizeof(newHeader));
                    newHeader.msg_type = EUSREXISTS;
                    newHeader.msg_len = 0;
                    if (wr_msg(*client_fd, &newHeader, NULL) < 0) {
                        printf("Write error\n");
                        exit(EXIT_FAILURE);
                    }
                    printf("Username already exists. Connection refused.\n");
                    
                    pthread_mutex_lock(&aLog.auditLogMutex);

                    FILE *file = fopen(aLog.fileName, "a");

                    time(&t);
                    fprintf(file, "User denied: %s, %d at %s\n", username, *client_fd, ctime(&t));
                    fclose(file);

                    pthread_mutex_unlock(&aLog.auditLogMutex);


                    pthread_mutex_unlock(&users.usersMutex);
                    pthread_mutex_unlock(&buffer_lock);
                    goto begin;
                }
                temp = temp->next;
            }
            petr_header newHeader;
            memset(&newHeader, 0, sizeof(newHeader));
            newHeader.msg_type = OK;
            newHeader.msg_len = 0;
            if (wr_msg(*client_fd, &newHeader, NULL) < 0) {
                printf("Write error\n");
                exit(EXIT_FAILURE);
            }
            struct user *newUser = malloc(sizeof(struct user));
            newUser->username = username;
            newUser->fd = *client_fd;
            
            if (users.userList == NULL)
                newUser->next = NULL;
            else
                newUser->next = users.userList;
            
            users.userList = newUser;

            
            printf("Client (%s) connection accepted\n", username);
            pthread_mutex_lock(&aLog.auditLogMutex);

            FILE* file = fopen(aLog.fileName, "a");
            time(&t);
            fprintf(file, "User accepted: %s, %d at %s\n", username, *client_fd, ctime(&t));
            pthread_create(&tid, NULL, process_client, (void *)newUser);
            time(&t);
            fprintf(file, "Client thread [%ld] created for %s at %s\n", (long)tid, username, ctime(&t));
            fclose(file);
            pthread_mutex_unlock(&aLog.auditLogMutex);
            pthread_mutex_unlock(&users.usersMutex);
            pthread_mutex_unlock(&buffer_lock);

        }
    }
    
    bzero(buffer, BUFFER_SIZE);
    close(listen_fd);
    
    return;
}

int main(int argc, char *argv[]) {
    int opt;
    int numJobs = 2;
    pthread_t tid;
    char *logFileName;

    unsigned int port = 0;
    while ((opt = getopt(argc, argv, "hj:")) != -1) {
        switch (opt) {
        case 'j':
            numJobs = atoi(optarg);
            break;
        case 'h':
        default: /* '?' */
            fprintf(stderr, "Server Application Usage: %s [-h][-j N] PORT_NUMBER AUDIT_FILENAME\n",
                    argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    port = atoi(argv[optind++]);
    logFileName = argv[optind];
    
    if (port == 0) {
        fprintf(stderr, "ERROR: Port number for server to listen is not given\n");
        fprintf(stderr, "Server Application Usage: %s [-h][-j N] PORT_NUMBER AUDIT_FILENAME\n",
                argv[0]);
        exit(EXIT_FAILURE);
    }

    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK_NP);

    pthread_mutex_init(&buffer_lock, &attr);
    pthread_mutex_init(&users.usersMutex, &attr);
    pthread_mutex_init(&rooms.roomListMutex, &attr);
    pthread_mutex_init(&jobs.jobQueueMutex, &attr);
    pthread_cond_init(&jobs.notEmpty, NULL);

    pthread_mutex_init(&aLog.auditLogMutex, &attr);

    users.userList = NULL;

    jobs.head = jobs.tail = NULL;
    jobs.size = 0;

    rooms.head = NULL;

    aLog.fileName = logFileName;

    for (int i = 0; i < numJobs; i++)
        pthread_create(&tid, NULL, process_job, NULL);

    run_server(port);
}