#include "proxy_parse.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>

#define MAX_CLIENTS 10

typedef struct cache_element cache_element;

struct cache_element
{
    char *data;            // data stream
    int len;               // size of data
    char *url;             // request url
    time_t lru_time_track; // how long this cache has been stored
    cache_element *next;   // next element
};

cache_element *find(char *url);                         // to find a cached result
int add_cache_element(char *data, int size, char *url); // to add a result to cache
void remove_cache_element();                            // to remove the longest stored cache

int port_number = 8080; // port for our socket
int proxy_socket_id;
pthread_t pid[MAX_CLIENTS];
sem_t semaphore;      // used for process synchronization
pthread_mutex_t lock; // same as semaphore only two values - on and off

cache_element *head;
int cache_size;

int main(int argc, char *const argv[])
{
    int client_socket_id, client_len;

    /* Definition for sockaddr, i.e., a descripter for a network address
        struct sockaddr{
            unsigned short sa_family; // Address family (AF_INET - IPv4, AF_INET6 - IPv6, AF_UNIX - local address)
            char sa_data[14]; // Address data
        }
    */
    struct sockaddr server_addr, client_addr;
    sem_init(&semaphore, 0, MAX_CLIENTS); // initializing a semaphore

    pthread_mutex_init(&lock, NULL); // initializing a mutex lock

    if(argc == 2){
        port_number = atoi(argv[1]);
    }else{
        printf("Too few arguements\n");
        exit(1);
    }

    printf("Starting proxy server at port: %d",port_number);

    

}
