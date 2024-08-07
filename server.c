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
#include <netdb.h>

#define MAX_CLIENTS 10
#define MAX_BYTES 4096
#define MAX_ELEMENT_SIZE 10 * (1 << 10)
#define MAX_SIZE 200 * (1 << 20)

typedef struct cache_element cache_element;
typedef struct ParsedRequest ParsedRequest;

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

/*
    The connectRemoteServer function establishes a TCP connection to a remote server with host address host_addr and port number port_num and returns the socket descriptor on success, or -1 on failure.
*/
int connectRemoteServer(char *host_addr, int port_num)
{
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0); // remote socket created by the socket() function
    if (remoteSocket < 0)                               // if socket creation was not successfull
    {
        printf("Error in create remote socket !\n");
        return -1;
    }
    struct hostent *host = gethostbyname(host_addr); // a structure containing host information
    if (host == NULL)                                // if the hostname resolution was unsuccessful
    {
        fprintf(stderr, "No such host exists\n");
        return -1;
    }

    struct sockaddr_in server_addr;                   // server address information
    bzero((char *)&server_addr, sizeof(server_addr)); // initializes the structure to zero
    server_addr.sin_family = AF_INET;                 // sets the address family to IPv4
    server_addr.sin_port = htons(port_num);           // sets the port number in network byte order

    bcopy((char *)&host->h_addr_list, (char *)&server_addr.sin_addr.s_addr, host->h_length);              // copies the IP address from host to the server_addr structure
    if (connect(remoteSocket, (const struct sockaddr *)&server_addr, (socklen_t)sizeof(server_addr)) < 0) // attempts to connect to the server using the specified socket, address, and length
    {
        fprintf(stderr, "Error in connecting\n"); // print error message if connection was unsuccessfull
        return -1;
    }
    return remoteSocket; // return the socket desciptor on success
}

/*
    The handle_request function handle's an incoming HTTP request, forwards it to a remote server and returns the response to the client. It also caches the response for potential future use.
    So basically client -> proxy_server -> server, back and forth
*/
int handle_request(int clientSocketId, ParsedRequest *request, char *tempReq)
{
    char *buf = (char *)malloc(sizeof(char) * MAX_BYTES); // buffer for storing the constructed HTTP request

    // constructs the request line by concatenating "GET ", the request path, a space, the HTTP version, and a newline character into the buffer.
    strcpy(buf, "GET ");
    strcat(buf, request->path);
    strcat(buf, " ");
    strcat(buf, request->version);
    strcat(buf, "\r\n");

    size_t len = strlen(buf); // Length of the constructed request line.

    if (ParsedHeader_set(request, "Connection", "close") < 0) // Sets the "Connection" header to "close" in the parsed request
    {
        printf("Set header key is not working !\n"); // Print error message if unsuccessfull
    }

    if (ParsedHeader_get(request, "Host") == NULL) // Checks if the "Host" header exists in the parsed request
    {
        if (ParsedHeader_set(request, "Host", request->host) < 0) // If not, sets it to the value of request->host
        {
            printf("Set Host header key is not working !\n"); // If unsuccessful, prints an error message.
        }
    }

    if (ParsedRequest_unparse_headers(request, buf + len, (size_t)(MAX_BYTES - len))) // appends the headers from request object to buffer
    {
        printf("Unparse Failed\n");
    }

    int server_port = 80;      // use default port as 80
    if (request->port != NULL) // if port is provided with the request
    {
        server_port = atoi(request->port); // then use the given port after converting it to integer
    }
    int remoteSocketId = connectRemoteServer(request->host, server_port); // connects to the remote server

    if (remoteSocketId < 0) // if connection to remote server fails
    {
        return -1;
    }
    int bytes_sent = send(remoteSocketId, buf, strlen(buf), 0); // send the constructed HTTP request to the remote server
    bzero(buf, MAX_BYTES);                                      // clears the buffer
    bytes_sent = recv(remoteSocketId, buf, MAX_BYTES - 1, 0);   // receive the data from the remote server and store it in the buffer, store the number of bytes received in bytes_sent

    char *temp_buffer = (char *)malloc(sizeof(char) * MAX_BYTES); // allocating a temporary buffer to store the response data for caching
    int temp_buffer_size = MAX_BYTES;                             // initial size of the temporary buffer
    int temp_buffer_index = 0;                                    // intialize the index for temporary buffer

    // we are sending data to client and receiving data from server and on and on
    while (bytes_sent > 0)
    {
        bytes_sent = send(clientSocketId, buf, bytes_sent, 0);   // sending data to client in chunks
        for (int i = 0; i < bytes_sent / (int)sizeof(char); i++) // Copy the data from buf to temp_buffer for caching.
        {
            temp_buffer[temp_buffer_index] = buf[i];
            temp_buffer_index++;
        }

        temp_buffer_size = MAX_BYTES;
        temp_buffer = (char *)realloc(temp_buffer, temp_buffer_size); // reallocate temp_buffer if needed
        if (bytes_sent < 0)                                           // checking if sending data to client failed
        {
            perror("Error in sending data to the client !");
            break;
        }
        bzero(buf, MAX_BYTES);                                    // clear the buffer
        bytes_sent = recv(remoteSocketId, buf, MAX_BYTES - 1, 0); // recieve more data from the remote server
    }

    temp_buffer[temp_buffer_index] = '\0';                        // null terminating the temp_buffer, it allows functions like printf and strlen to know where the string ends.
    free(buf);                                                    // free the allocated buffer
    add_cache_element(temp_buffer, strlen(temp_buffer), tempReq); // adds the entire response to the cache
    free(temp_buffer);                                            // free the temporary buffer
    close(remoteSocketId);                                        // close the connection to the remote server

    return 0;
};

/*
    The sendErrorMessage function constructs and sends an HTTP error response based on a given status code to a specified socket.
*/
int sendErrorMessage(int socket, int status_code)
{
    char str[1024];       // a buffer to store the error message
    char currentTime[50]; // a buffer to store the current time in a formatted string
    time_t now = time(0); // current time in seconds since the UNIX epoch

    struct tm data = *gmtime(&now);                                                // converts the UNIX timestamp to a broken down structure tm
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data); // formats the broken down structure to a human readable format and stores it in currentTime

    switch (status_code)
    {
    case 400:
        snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
        printf("400 Bad Request\n");
        send(socket, str, strlen(str), 0);
        break;

    case 403:
        snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
        printf("403 Forbidden\n");
        send(socket, str, strlen(str), 0);
        break;

    case 404:
        snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
        printf("404 Not Found\n");
        send(socket, str, strlen(str), 0);
        break;

    case 500:
        snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
        printf("500 Internal Server Error\n");
        send(socket, str, strlen(str), 0);
        break;

    case 501:
        snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
        printf("501 Not Implemented\n");
        send(socket, str, strlen(str), 0);
        break;

    case 505:
        snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
        printf("505 HTTP Version Not Supported\n");
        send(socket, str, strlen(str), 0);
        break;

    default:
        return -1;
    }
    return 1;
}

// checks HTTP version
int checkHTTPversion(char *msg)
{
    int version = -1;
    if (strncmp(msg, "HTTP/1.1", 8) == 0 || strncmp(msg, "HTTP/1.0", 8) == 0)
    {
        version = 1;
    }
    return version;
}

/*
    The thread_fn function handles the incoming client requests in a separate thread. It handles request parsing, caching, forwarding, and error handling.
*/
void *thread_fn(void *socketNew)
{
    sem_wait(&semaphore); // acquires the semaphore, ensuring only one thread can execute at a time
    int p;
    sem_getvalue(&semaphore, &p);         // stores the current value of semaphore in p
    printf("Semaphore value is %d\n", p); // print the semaphore value

    int *t = (int *)socketNew;     // cast the void pointer to an integer pointer
    int socket = *t;               // dereference the pointer to get the socket ID
    int bytes_sent_by_client, len; // data sent by client and its length

    char *buffer = (char *)calloc(MAX_BYTES, sizeof(char));    // allocating memory for the buffer to store received data
    bzero(buffer, MAX_BYTES);                                  // clear the buffer
    bytes_sent_by_client = recv(socket, buffer, MAX_BYTES, 0); // receive the data from the client

    // loop to receive the complete HTTP request (until the end of headers "\r\n\r\n")
    while (bytes_sent_by_client > 0)
    {
        len = strlen(buffer);                   // get the length of the current buffer
        if (strstr(buffer, "\r\n\r\n") == NULL) // if the end of header is not found
        {
            bytes_sent_by_client = recv(socket, buffer + len, MAX_BYTES, 0); // then receive more data
        }
        else
        {
            break; // end of headers found, break the loop
        }
    }

    // A copy of the recieved request for caching purpose
    char *tempReq = (char *)malloc(strlen(buffer) * sizeof(char));
    for (int i = 0; i < (int)strlen(buffer); i++)
    {
        tempReq[i] = buffer[i];
    }

    struct cache_element *temp = find(tempReq); // find the request in the cache
    if (temp != NULL)                           // If the request is found in cache
    {
        int size = temp->len / sizeof(char); // length  of the cached data
        int pos = 0;                         // position index for sending data
        char response[MAX_BYTES];            // response buffer
        while (pos < size)
        {
            bzero(response, MAX_BYTES); // clear the response buffer
            for (int i = 0; i < MAX_BYTES; i++)
            {
                response[i] = temp->data[1]; // copy data from cache to response buffer
                pos++;
            }
            send(socket, response, MAX_BYTES, 0); // send the cached data to the client
        }
        printf("Data retrieved from the catche\n");
        printf("%s\n\n", response);
    }
    else if (bytes_sent_by_client > 0) // If the request was not found in cache but we recieved request/bytes from the client successfully
    {
        len = strlen(buffer);                              // length of the buffer
        ParsedRequest *request = ParsedRequest_create();   // create a ParsedRequest object
        if (ParsedRequest_parse(request, buffer, len) < 0) // parse the request in a readable format
        {
            printf("Parsing failed \n"); // if the parsing fails
        }
        else
        {
            bzero(buffer, MAX_BYTES);            // clearing the buffer
            if (!strcmp(request->method, "GET")) // If the request method is GET
            {
                if (request->host && request->path && checkHTTPversion(request->version) == 1) // If host is valid  and URL path is valid and the HTTP version is 1
                {
                    bytes_sent_by_client = handle_request(socket, request, tempReq); // Handle the request
                    if (bytes_sent_by_client == -1)
                    {
                        sendErrorMessage(socket, 500); // send an error if the request handling failed
                    }
                }
                else
                {
                    sendErrorMessage(socket, 500); // send an error message if the host or path or HTTP version could not be validated
                }
            }
            else
            {
                printf("This code doesn't support any method apart from GET\n"); // if the method something else than GET
            }
        }
        ParsedRequest_destroy(request); // destroy the ParsedRequest object
    }
    else if (bytes_sent_by_client == 0)
    {
        printf("Client is disconnected\n"); // print message if client is disconnected
    }
    shutdown(socket, SHUT_RDWR);               // Shut down a socket, SHUT_RDWR -> terminate both reading and writing operations
    close(socket);                             // close the socket
    free(buffer);                              // free the buffer
    sem_post(&semaphore);                      // increment the semaphore value, making it available
    sem_getvalue(&semaphore, &p);              // get the current value of semaphore
    printf("Semaphore post value is %d\n", p); // print the current value of semaphore
    free(tempReq);                             // free the tempReq buffer
    return NULL;                               // return NULL
}

int main(int argc, char *const argv[])
{
    int client_socket_id, client_len; // client socket ID and client address length

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
    struct sockaddr_in server_addr, client_addr; // structures to store client and server information
    sem_init(&semaphore, 0, MAX_CLIENTS);        // initializing a semaphore with initial value MAX_CLIENTS

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

    printf("Starting proxy server at port: %d\n", port_number);

    /*
        The function socket(int domain, int type, int protocol) creates a new socket with
        the address family AF_INET (IPv4), and socket type SOCK_STREAM (TCP), and protocol
        which is set to 0 for default protocol.
    */
    proxy_socket_id = socket(AF_INET, SOCK_STREAM, 0);

    if (proxy_socket_id < 0)
    {
        printf("Failed to create a socket !!\n");
        exit(1);
    }

    int reuse = 1;
    if (setsockopt(proxy_socket_id, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0) // set the socket options to reuse the address
    {
        perror("setSockOpt Failed \n");
    }

    // Writes 0's in server_addr to replace garbage value (clearing the server_addr structure)
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
        perror("Port is not available !!"); // exit if binding fails
        exit(1);
    }
    printf("Binding on port %d\n", port_number);

    int listen_status = listen(proxy_socket_id, MAX_CLIENTS); // sets the socket to listen for incoming connections
    if (listen_status < 0)
    {
        perror("Error in listening \n");
        exit(1);
    }

    int i = 0;                           // initializing current number of client connections
    int Connectd_socket_id[MAX_CLIENTS]; // array for connected socket ID's

    while (1)
    {
        bzero((char *)&client_addr, sizeof(client_addr)); // clear the client address structure
        client_len = sizeof(client_addr);                 // set the size of client address

        // creates new socket for communication between listening socket and client
        client_socket_id = accept(proxy_socket_id, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);
        if (client_socket_id < 0)
        {
            printf("Not able to connect !!\n");
            exit(1); // exit if accepting a connection fails
        }
        else
        {
            Connectd_socket_id[i] = client_socket_id; // store the clients socket ID
        }

        struct sockaddr_in *client_pt = (struct sockaddr_in *)&client_addr; // creating a copy
        struct in_addr ip_addr = client_pt->sin_addr;                       // getting IP address of the client
        char str[INET_ADDRSTRLEN];

        // converts numeric IP address to text
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);

        printf("Client is connected with port number %d and IP address %s\n", ntohs(client_addr.sin_port), str);

        // create a new thread to hanlde the clients request
        pthread_create(&tid[i], NULL, thread_fn, (void *)&Connectd_socket_id[i]);
        i++;
    }
    close(proxy_socket_id); // close the proxy socket
    return 0;
}

// finds and returns an element in the cache
cache_element *find(char *url)
{
    cache_element *site = NULL;
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Remove Cache Lock acquired %d\n", temp_lock_val);

    if (head != NULL)
    {
        site = head;
        while (site != NULL)
        {
            if (!strcmp(site->url, url))
            {
                printf("LRU time track before : %d\n", (int)(site->lru_time_track));
                printf("\n Url found\n");
                site->lru_time_track = time(NULL);
                printf("LRU time track after %d\n", (int)(site->lru_time_track));
                break;
            }
            site = site->next;
        }
    }
    else
    {
        printf("URL not found\n");
    }
    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("Lock is unlocked\n");
    return site;
}

// Search for element with least lru_time_track and remove it
void remove_cache_element()
{
    cache_element *p;
    cache_element *q;
    cache_element *temp;

    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Lock is acquired \n");

    // linked list type iteration
    if (head != NULL)
    {
        for (p = head, q = head, temp = head; q->next != NULL; q = q->next)
        {
            if ((q->next)->lru_time_track < temp->lru_time_track)
            {
                temp = q->next;
                p = q;
            }
        }
        if (temp == head)
        {
            head = head->next;
        }
        else
        {
            p->next = temp->next;
        }
        cache_size = cache_size - (temp->len) - sizeof(cache_element) - strlen(temp->url) - 1;
        free(temp->data);
        free(temp->url);
        free(temp);
    }
    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("Remove mutex lock\n");
}

int add_cache_element(char *data, int size, char *url)
{
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Add Cache Lock acquired %d\n", temp_lock_val);

    int element_size = size + 1 + strlen(url) + sizeof(cache_element);
    if (element_size < MAX_ELEMENT_SIZE)
    {
        temp_lock_val = pthread_mutex_unlock(&lock);
        printf("Add cache lock is unlocked\n");
        return 0;
    }
    else
    {
        while (cache_size + element_size > MAX_SIZE)
        {
            remove_cache_element();
        }
        cache_element *element = (cache_element *)malloc(sizeof(cache_element));
        element->data = (char *)malloc(size + 1);
        strcpy(element->data, data);
        element->url = (char *)malloc(strlen(url) + sizeof(char) + 1);
        strcpy(element->url, url);
        element->lru_time_track = time(NULL);
        element->next = head;
        element->len = size;
        head = element;
        cache_size += element_size;
        temp_lock_val = pthread_mutex_unlock(&lock);
        printf("Add Cache lock is unlocked\n");
        return 1;
    }
    return 0;
}
