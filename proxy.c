/**
 * @file proxy.c
 * @brief A simple HTTP proxy server implementation
 * 
 * This program implements a simple multithreaded HTTP proxy server which is able to
 * handle HTTP requests from clients, forward those requests to the appropriate web
 * servers, and return the responses to the clients. The program uses POSIX threads
 * to handle concurrency as well as the CS:APP RIO package for robust I/O operations.
 * As of right now, the proxy server only supports the GET method. Additionally, when
 * forwarded to the target servers, incoming HTTP/1.1 requests are translated to HTTP/1.0.
 *
 * Key Points:
 * - Parses HTTP requests using the HTTP parsing library
 * - Manages connections between client and server with sockets
 * - Handles multiple connections (at the same time) with POSIX threads
 * 
 * Design Decisions:
 * - Uses robust I/O functions from the CS:APP package to manage reading 
 *   and writing operations for proper error handling
 * - Handles concurrent requests using POSIX threads
 * 
 * Limitations:
 * - Only supports HTTP GET requests
 *
 * @author Lisa Huang <wenleh@andrew.cmu.edu>
 */

#include "csapp.h"
#include "http_parser.h"
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * Debug macros, which can be enabled by adding -DDEBUG in the Makefile
 * Use these if you find them useful, or delete them if not
 */
#ifdef DEBUG
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_assert(...)
#define dbg_printf(...)
#endif

#define HTTP_PREFIX_LEN 7
#define HOST_HEADER_LEN 5
#define CONN_HEADER_LEN 11
#define USER_AGENT_HEADER_LEN 11
#define PROXY_CONN_HEADER_LEN 17

/*
 * String for the User-Agent header
 */
static const char *header_user_agent = "User-Agent: Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20240719 Firefox/63.0.1\r\n";

/**
 * @brief Cleans up resources used during request processing
 * 
 * This function releases resources which were allocated during the processing
 * of an HTTP request, closing the server socket and freeing the HTTP parser.
 * This helps to prevent memory/resource leaks after requests.
 *
 * @param[in] server - Server connection's file descriptor
 * @param[in] parser - A pointer to the parser_t struct
 *
 */
void cleanUp(int server,parser_t* parser) {
    close(server);
    parser_free(parser);
}

/**
 * @brief Returns an error message to the client
 * 
 * @param[in] fd - Client connection's file descriptor
 * @param[in] errnum - The HTTP status code
 * @param[in] shortmsg - A short message describing the error
 * @param[in] longmsg - A detailed message describing the error
 * 
 */
void clienterror(int fd, const char *errnum, const char *shortmsg,
                 const char *longmsg) {
    char buf[MAXLINE];
    char body[MAXBUF];
    size_t buflen;
    size_t bodylen;

    /* Build the HTTP response body */
    bodylen = snprintf(body, MAXBUF,
            "<!DOCTYPE html>\r\n" \
            "<html>\r\n" \
            "<head><title>Tiny Error</title></head>\r\n" \
            "<body bgcolor=\"ffffff\">\r\n" \
            "<h1>%s: %s</h1>\r\n" \
            "<p>%s</p>\r\n" \
            "<hr /><em>The Tiny Web server</em>\r\n" \
            "</body></html>\r\n", \
            errnum, shortmsg, longmsg);
    if (bodylen >= MAXBUF) {
        return; // Overflow!
    }

    /* Build the HTTP response headers */
    buflen = snprintf(buf, MAXLINE,
            "HTTP/1.0 %s %s\r\n" \
            "Content-Type: text/html\r\n" \
            "Content-Length: %zu\r\n\r\n", \
            errnum, shortmsg, bodylen);
    if (buflen >= MAXLINE) {
        return; // Overflow!
    }

    /* Write the headers */
    if (rio_writen(fd, buf, buflen) < 0) {
        fprintf(stderr, "Error writing error response headers to client\n");
        return;
    }

    /* Write the body */
    if (rio_writen(fd, body, bodylen) < 0) {
        fprintf(stderr, "Error writing error response body to client\n");
        return;
    }
}

/**
 * @brief Parses the given URI to extract the host, port, and path
 * 
 * This function parses the given URI and extracts the host, port, and path components. It is
 * able to handle URIs which have or doesn't have specified ports (defaults to port 80 if 
 * none provided).
 * 
 * @param[in] uri - The URI string to be parsed
 * @param[out] path - Buffer to store extracted path
 * @param[out] host - Buffer to store extracted host
 * @param[out] port - Buffer to store extracted port
 * 
 * @return Returns 0 on success
 *
 */
int processUri(char* uri,char* path,char* host, char *port) {
    char* start = uri+HTTP_PREFIX_LEN;
    char* end = start;
    while (*end && *end!=':'&& *end!='/' && *end !=' ') {
        end++;
    }
    strncpy(host,start,end-(start));
    host[end-(start)] = '\0';
    if (*end==':') {
        char* temp=end+1;
        while (*end&&*end != '/'&&*end != ' ') {
            end++;
        }
        strncpy(port, temp, end-temp);
        port[end-temp] = '\0';
    } else {
        strcpy(port,"80");
    }
    if (*end=='/') {
        strncpy(path,end, MAXLINE);
    } else {
        path[0]='\0';
    }
    return 0;
}

/**
 * @brief Reads lines from the client, filters specific headers, and forwards 
 * the rest to the server
 *
 * @param[in] buf - Buffer to store line read
 * @param[in] temp - Initial number of bytes read into the buffer
 * @param[in] client - rio_t struct representing the client's buffered input
 * @param[in] server - Server connection's file descriptor
 * 
 * @return Returns true if all lines processed successfully, returns false if error
 * 
 */
bool readLine(char* buf,int temp, rio_t client,int server) {
    while(temp>0) {
        if (strcmp(buf, "\r\n")==0) {
            break;
        }
        bool skipped=false;
        if (strncmp(buf, "Host:", HOST_HEADER_LEN)==0) skipped=true;
        if (strncmp(buf,"Connection:",CONN_HEADER_LEN)==0) skipped = true;
        if (strncmp(buf,"User-Agent:",USER_AGENT_HEADER_LEN)==0) skipped = true;
        if (strncmp(buf,"Proxy-Connection:", PROXY_CONN_HEADER_LEN)==0) skipped = true;
        if (skipped ==false) {
            if (rio_writen(server, buf, strlen(buf))<0) {
                close(server);
                return false;
            }
        }
        temp=rio_readlineb(&client, buf, MAXLINE);
    }
    return true;
}

/**
 * @brief Handles HTTP request from client
 * 
 * This function reads HTTP request from the client, parses the request, and 
 * forwards it to the web server. Then, it reads and sends the server's response
 * to the client. The function uses robust I/O operations to handle errors properly.
 * 
 * @param[in] fd - Client connection's file descriptor
 *
 */
void request(int fd) {
    const char *method;
    const char *uri;
    const char *vers;
    rio_t client;
    rio_t server;
    char buf[MAXLINE];
    char path[MAXLINE];
    char host[MAXLINE];
    char port[MAXLINE];
    char request[MAXLINE];
    int serverFd=-1;
    rio_readinitb(&client,fd);
    parser_t* parser =parser_new();
    parser_state state;
    
    if (rio_readlineb(&client,buf,MAXLINE)==false) {
        parser_free(parser);
        return;
    }

    //Attempts to parse the request line to extract method, URI, and version
    state = parser_parse_line(parser,buf);
    if (state != REQUEST) {
        parser_free(parser);
        return;
    }

    if(parser_retrieve(parser, METHOD,&method)<0 || 
            parser_retrieve(parser, URI,&uri)<0 || 
                parser_retrieve(parser, HTTP_VERSION,&vers)<0) {
        parser_free(parser);
        return;
    }

    //Only the GET method is supported
    if (strcmp(method, "GET")!=0) {
        clienterror(fd,"501","Not Implemented","Tiny does not implement this method");
        parser_free(parser);
        return;
    }

    //Attempts to process the URI to extract host, path, and port
    bool first = processUri((char*)uri, path,host,port)<0;

    //Attempts to establish connection with server
    bool second = (serverFd=open_clientfd(host, port))<0;
    if (first||second) {
        parser_free(parser);
        return;
    }
    rio_readinitb(&server, serverFd);

    first = snprintf(request,MAXLINE, "GET %s HTTP/1.0\r\n", path)>=MAXLINE;
    second = rio_writen(serverFd,request,strlen(request))<0;
    if (first||second) {
        cleanUp(serverFd, parser);
        return;
    }

    bool saved = snprintf(buf, MAXLINE,"Host: %s:%s\r\n", host,port)>=MAXLINE;
    if (saved) {
        cleanUp(serverFd, parser);
        return;
    }

    //Attempts to forward User-Agent, Connection, and Proxy-Connection headers to the server
    if (rio_writen(serverFd,buf,strlen(buf)) < 0 ||
            rio_writen(serverFd,header_user_agent,strlen(header_user_agent))<0 || 
                rio_writen(serverFd, "Connection: close\r\n", strlen("Connection: close\r\n"))<0 || 
                    rio_writen(serverFd, "Proxy-Connection: close\r\n", strlen("Proxy-Connection: close\r\n"))<0) {
        cleanUp(serverFd, parser);
        return;
    }

    int temp=rio_readlineb(&client,buf, MAXLINE);
    if(readLine(buf,temp,client, serverFd)==false) {
        parser_free(parser);
        return;
    }
    if (rio_writen(serverFd,"\r\n", 2)<0) {
        cleanUp(serverFd, parser);
        return;
    }

    size_t tempAgain;
    bool stillRun=true;

    //Read and forward additional client headers 
    while (stillRun &&(tempAgain=rio_readnb(&server, buf, MAXLINE))>0) {
        if (rio_writen(fd, buf, tempAgain)!=tempAgain) {
            stillRun=false;
        }
    }
    cleanUp(serverFd,parser);
}

/**
 * @brief Signal handler for SIGPIPE
 * 
 * This function does nothing intentionally; it is used to suppress SIGPIPE signals.
 */
void handle() {
    //Do nothing
}

/**
 * @brief Thread function for handling client connections
 * 
 * @param[in] arg - A pointer to the client connection's allocated integer file descriptor
 * 
 * @return Returns NULL as per the pthread standard
 */
void *thread(void* arg) {
    int conn = *(int*)arg;
    pthread_detach(pthread_self());
    free(arg);
    request(conn);
    close(conn);

    return NULL;
}

/**
 * @brief Main server loop to handle client connections
 * 
 * This function listens for new client connections on the specified listening socket.
 * 
 * @param[in] listen - Listening socket's file descriptor
 *
 */
void run (int listen) {
    int *conn=NULL;
    char host[MAXLINE];
    char port[MAXLINE];
    struct sockaddr_storage address;
    socklen_t len=0;
    pthread_t tid;
    while(true) {
        len=sizeof(address);
        conn = malloc(sizeof(int));
        *conn =accept(listen, (struct sockaddr*)&address,&len);
        getnameinfo((struct sockaddr*)&address,len,host,MAXLINE,port,MAXLINE,0);
        pthread_create(&tid,NULL,thread,conn);
    }
}

/**
 * @brief Entry point for the HTTP proxy server
 * 
 * This function initializes the HTTP proxy server by setting up signal handling, 
 * opening a listening socket, and starting the server's main loop.
 * 
 * @param[in] argc - The number of command line arguments
 * @param[in] argv - An array of command line argument strings
 * 
 * @return Returns 0 on successful execution and exits if incorrect number of arguments
 */
int main(int argc, char **argv) {
    if (argc!=2) {
        exit(1);
    }
    signal(SIGPIPE, handle);
    int listen=open_listenfd(argv[1]);
    run(listen);
    close(listen);
    return 0;
}
