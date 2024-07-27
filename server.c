#include "proxy_parse.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define MAX_CLIENTS 10
#define MAX_BYTES 4096

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
pthread_t tid[MAX_CLIENTS];
sem_t semaphore;      // used for process synchronization
pthread_mutex_t lock; // same as semaphore only two values - on and off

cache_element *head;
int cache_size;

void *thread_fn(void *socketNew)
{
    sem_wait(&semaphore);
    int p;
    sem_getvalue(&semaphore, p);
    printf("Semaphore value is %d\n", p);

    int *t = (int *)socketNew;
    int socket = *t;
    int bytes_send_client, len; // data send by client and its length

    char *buffer = (char *)calloc(MAX_BYTES, sizeof(char));
    bzero(buffer, MAX_BYTES);
    bytes_send_client = recv(socket, buffer, MAX_BYTES, 0);

    while (bytes_send_client > 0)
    {
        len = strlen(buffer);
        if (strstr(buffer, "\r\n\r\n") == NULL)
        {
            bytes_send_client = recv(socket, buffer + len, MAX_BYTES, 0);
        }
        else
        {
            break;
        }
    }

    // Allocates some memory to a character array and returns a pointer to that array
    char *tempReq = (char *)malloc(strlen(buffer) * sizeof(char));
    for (int i = 0; i < strlen(buffer); i++)
    {
        tempReq[i] = buffer[i];
    }
    struct cache_element *temp = find(tempReq);
    if (temp != NULL)
    {
        int size = temp->len / sizeof(char);
        int pos = 0;
        char response[MAX_BYTES];
        while (pos < size)
        {
            bzero(response, MAX_BYTES);
            for (int i = 0; i < MAX_BYTES; i++)
            {
                response[1] = temp->data[1];
                pos++;
            }
            send(socket, response, MAX_BYTES, 0);
        }
        printf("Data retrieved from the catche\n");
        printf("%s\n\n",response);
    }
    else if(bytes_send_client > 0){
        len = strlen(buffer);
        struct ParsedRequest *request = ParsedRequest_create();
        if(ParsedRequest_parse(request, buffer, len) < 0){
            printf("Parsing failed \n");
        }
        else{
            bzero(buffer, MAX_BYTES);
            if(!strcmp(request->method, "GET")){
                if(request->host && request->path && checkHTTPversion(request->version) == 1){
                    bytes_send_client = handle_request(socket, request, tempReq);
                    if(bytes_send_client == -1){
                        sendErrorMessage(socket, 500);
                    }
                }else{
                    sendErrorMessage(socket, 500);
                }
            }else{
                printf("This code doesn't support any method apart GET\n");
            }
        }
        ParsedRequest_destroy(request);
    }
}

int main(int argc, char *const argv[])
{
    int client_socket_id, client_len;

    /*
        Definition for sockaddr, i.e., a descripter for a generic network address
        struct sockaddr{
            unsigned short sa_family; // Address family (AF_INET - IPv4, AF_INET6 - IPv6, AF_UNIX - local address)
            char sa_data[14]; // Address data
        }

        Definition for sockaddr_in, i.e., a descripter for an Internet network address
        struct sockaddr_in {
            short int          sin_family;   // Address family (AF_INET)
            unsigned short int sin_port;     // Port number
            struct in_addr     sin_addr;     // Internet address
            unsigned char      sin_zero[8];  // Padding to make the structure the same size as `struct sockaddr`
        };

    */
    struct sockaddr_in server_addr, client_addr;
    sem_init(&semaphore, 0, MAX_CLIENTS); // initializing a semaphore

    pthread_mutex_init(&lock, NULL); // initializing a mutex lock

    if (argc == 2)
    {
        port_number = atoi(argv[1]); // Use port number if given
    }
    else
    {
        printf("Too few arguements\n");
        exit(1);
    }

    printf("Starting proxy server at port: %d", port_number);

    /*
        The function socket(int domain, int type, int protocol) creates a new socket with
        the address family AF_INET (IPv4), and socket type SOCK_STREAM (TCP), and protocol
        which is set to 0 for default protocol.
    */
    proxy_socket_id = socket(AF_INET, SOCK_STREAM, 0);

    if (proxy_socket_id < 0)
    {
        printf("Failed to create a socket !!");
        exit(1);
    }

    int reuse = 1;
    if (setsockopt(proxy_socket_id, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0)
    {
        perror("setSockOpt Failed \n");
    }

    // Writes 0's in server_addr to replace garbage value
    bzero((char *)&server_addr, sizeof(server_addr));

    // Assiging values to the server address
    server_addr.sin_family = AF_INET; // IPv4

    // converting port_number from host byte order to network byte order
    server_addr.sin_port = htons(port_number);

    // accept connection from any IP address
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Binding the socket proxy_socket_id with the Address server_addr
    if (bind(proxy_socket_id, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Port is not available !!");
        exit(1);
    }
    printf("Binding on port %d\n", port_number);

    int listen_status = listen(proxy_socket_id, MAX_CLIENTS); // socket is ready to accept connections
    if (listen_status < 0)
    {
        perror("Error in listening \n");
        exit(1);
    }

    int i = 0;
    int Connectd_socket_id[MAX_CLIENTS];

    while (1)
    {
        bzero((char *)&client_addr, sizeof(client_addr));
        client_len = sizeof(client_addr);

        // creates new socket for communication between listening socket and client
        client_socket_id = accept(proxy_socket_id, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);
        if (client_socket_id < 0)
        {
            printf("Not able to connect !!");
            exit(1);
        }
        else
        {
            Connectd_socket_id[1] = client_socket_id;
        }

        struct sockaddr_in *client_pt = (struct sockaddr_in *)&client_addr; // creating a copy
        struct in_addr ip_addr = client_pt->sin_addr;                       // getting ip address of the client
        char str[INET_ADDRSTRLEN];

        // converts numeric IP address to their corresponding text representations
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);

        printf("Client is connected with port number %d and IP address %s\n", ntohs(client_addr.sin_port), str);

        pthread_create(&tid[i], NULL, thread_fn, (void *)&Connectd_socket_id[i]);
        i++;
    }
    close(proxy_socket_id);
    return 0;
}
