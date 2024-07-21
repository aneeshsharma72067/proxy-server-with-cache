#include "proxy_parse.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

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

int port = 8080; // port for our socket
int proxy_socket_id;
pthread_t pid[MAX_CLIENTS];
// sem_t semaphore;
pthread_mutex_t lock;