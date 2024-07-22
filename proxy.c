/*
 * Starter code for proxy lab.
 * Feel free to modify this code in whatever way you wish.
 */

/* Some useful includes to help you get started */

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


/*
 * String to use for the User-Agent header.
 * Don't forget to terminate with \r\n
 */
static const char *header_user_agent = "User-Agent: Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20240719 Firefox/63.0.1\r\n";

void cleanUp(int server,parser_t* parser) {
    close(server);
    parser_free(parser);
}

/*
 * clienterror - returns an error message to the client
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

int processUri(char* uri,char* path,char* host, char *port) {
    char* end = uri+7;
    while (*end && *end!=':'&& *end!='/' && *end !=' ') {
        end++;
    }
    strncpy(host,uri+7,end-(uri+7));
    host[end-(uri+7)] = '\0';
    if (*end==':') {
        char* temp=end + 1;
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

bool readLine(char* buf,int temp, rio_t client,int server) {
    while(temp>0) {
        if (strcmp(buf, "\r\n")==0) {
            break;
        }
        bool skipped=false;
        if (strncmp(buf, "Host:", 5)==0) skipped=true;
        if (strncmp(buf,"Connection:",11)==0) skipped = true;
        if (strncmp(buf,"User-Agent:",11)==0) skipped = true;
        if (strncmp(buf,"Proxy-Connection:", 17)==0) skipped = true;
        if (skipped ==false) {
            if (rio_writen(server, buf, strlen(buf))<0) {
                close(server);
                return false;
            }
        }
        temp=rio_readlineb(&client, buf, MAXLINE);
    }
    //printf("readLine done\n");
    return true;
}

void request(int fd) {
    const char *method;
    const char *uri;
    const char *vers;
    rio_t client;
    rio_t server;//=NULL;
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
    state = parser_parse_line(parser,buf);
    if (state != REQUEST) {
        parser_free(parser);
        return;
    }

    //printf("hiHIHI\n");
    if(parser_retrieve(parser, METHOD,&method)<0 || 
            parser_retrieve(parser, URI,&uri)<0 || 
                parser_retrieve(parser, HTTP_VERSION,&vers)<0) {
        parser_free(parser);
        return;
    }

    if (strcmp(method, "GET")!=0) {
        clienterror(fd,"501","Not Implemented","Tiny does not implement this method");
        parser_free(parser);
        return;
    }

    bool first = processUri((char*)uri, path,host,port)<0;
    bool second = (serverFd=open_clientfd(host, port))<0;
    if (first||second) {
        parser_free(parser);
        return;
    }
    rio_readinitb(&server, serverFd);

    first = snprintf(request,MAXLINE, "GET %s HTTP/1.0\r\n", path)>=MAXLINE;
    second = rio_writen(serverFd,request,strlen(request))<0;
    if (first||second) {
        //printf("yes\n");
        cleanUp(serverFd, parser);
        return;
    }

    bool saved = snprintf(buf, MAXLINE,"Host: %s:%s\r\n", host,port)>=MAXLINE;
    if (saved) {
        cleanUp(serverFd, parser);
        return;
    }
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

    while (stillRun &&(tempAgain=rio_readnb(&server, buf, MAXLINE))>0) {
        if (rio_writen(fd, buf, tempAgain)!=tempAgain) {
            stillRun=false;
        }
    }
    //printf("yesyes\n");
    cleanUp(serverFd,parser);
}


void handle() {
    //do nothing
}

void run (int listen) {
    //printf("here\n");
    int conn=-1;
    char host[MAXLINE];
    char port[MAXLINE];
    struct sockaddr_storage address;
    socklen_t len=0;
    while(true) {
        len=sizeof(address);
        conn =accept(listen, (struct sockaddr*)&address,&len);
        getnameinfo((struct sockaddr*)&address,len,host,MAXLINE,port,MAXLINE,0);
        request(conn);
        close(conn);
    }
}

int main(int argc, char **argv) {
    if (argc!=2) {
        exit(1);
    }
    //printf("hi main\n");
    signal(SIGPIPE, handle);
    int listen=open_listenfd(argv[1]);
    run(listen);
    close(listen);
    return 0;
}
