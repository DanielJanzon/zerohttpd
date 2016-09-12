/*
   ZeroHTTP - Bare minimal high performance HTTP Server
   Author   - Daniel Janzon
*/


#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <fcntl.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <event.h>

#include "http-parser/http_parser.h"

#define D(x)

struct event; /* Forward declaration */
typedef void (*event_callback)(int sockfd, void *priv);

struct client
{
  struct event read_event;
  struct event continue_event;
  http_parser parser; /* Used to parse HTTP requests */
  http_parser_settings parser_settings;
  char url[1024];
  int fd; /* File that we are currently streaming */
  off_t bytes_sent;
  int file_size;
  int sockfd;
};


struct event rate_counter_event;
static int rate_counter_counter = 0;

enum http_return_code {HTTP_200_OK = 0, HTTP_404_NOT_FOUND};
const char * http_return_string[] = {"200 OK", "404 Not Found"};

/* Write the HTTP document header */
int write_header(int socket, enum http_return_code return_code, int content_length)
{
  const char *head_fmt = "HTTP/1.1 %s\r\nContent-Length: %d\r\n\r\n";
  char head[128];

  int n = snprintf(head, sizeof(head), head_fmt, http_return_string[return_code], content_length);
  int m = write(socket, head, n);
  return m;
}

void serve_file_continue(int sockfd, short what, void *arg)
{
  struct client *client = arg;

  D(printf("continue streaming on client %p\n", client);)
  int bytes_to_send =  client->file_size - client->bytes_sent;

  int n = sendfile(sockfd, client->fd, 0, bytes_to_send);

  if (n < 0 && errno != EAGAIN)
  {
    D(printf("closing client %p\n", client);)
    close(client->fd);
    return;
  }

  if( (n < 0 && errno == EAGAIN) || n < bytes_to_send) {
    D(printf("%d bytes were sent\n", (int)n);)
    #warning "Need to call event_set again?"
    event_set(&client->continue_event, client->sockfd, EV_WRITE, serve_file_continue, client);
    client->bytes_sent += n;
    event_add(&client->continue_event, NULL);
  }
  else
  {
    D(printf("closing client %p\n", client);)
    close(client->fd);
  }
}

int serve_file(struct client *client, const char *filename)
{
  D(printf("serve_file: opening file '%s'\n", filename);)
  client->fd = open(filename, O_RDONLY);
  if(client->fd < 0)
  {
    write_header(client->sockfd, HTTP_404_NOT_FOUND, 0);
    return 0; /* 404 is totally legitimate */
  }

  struct stat sb;
  fstat(client->fd, &sb);
  client->file_size = sb.st_size;
  const char *head_fmt = "HTTP/1.1 %s\r\nContent-Length: %d\r\n\r\n";
  char head[128];
  int hdr_size = snprintf(head, sizeof(head), head_fmt, http_return_string[HTTP_200_OK], client->file_size);

  if (write(client->sockfd, head, hdr_size) < hdr_size)
  {
      printf("oh dear!\n");
  }

  serve_file_continue(client->sockfd, EV_WRITE, client);

  return 0;
}

int zero_on_message_complete(http_parser *parser)
{
  D(printf("got message complete on event %p\n", parser->data);)
  struct client *client = (struct client*)parser->data;
  char *sp = client->url;
  while(*sp == '/')
    sp++;

  int err = serve_file(client, sp);
  rate_counter_counter++;
  return err; /* Hopefully tell parser all went ok */
}

int zero_on_url(http_parser *parser, const char *at, size_t length)
{
  struct client *client = (struct client*)parser->data;
  D(printf("zero_on_url: got length %d\n", (int)length);)
  length = (length < 1023) ? length : 1023;
  memcpy(client->url, at, length);
  client->url[length] = '\0';
  D(printf("got url '%s' on client %p\n", client->url, client);)

  return 0; /* Tell parser all went ok */
}

	
struct http_config
{
  int listen_port;
  int max_pending;
  int client_buf_size;
} cfg;


static struct event new_con_ev;

static inline void set_nonblocking(int fd)
{
  #warning "no error checking on fcntl return"
  int flags = fcntl(fd, F_GETFL, 0);
  flags |= O_NONBLOCK;
  fcntl(fd, F_SETFL, O_NONBLOCK);
}

static inline void set_no_tcp_delay(int fd)
{
  int flag = 1;
  int err = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));
  if(err)
  {
    fprintf(stderr, "fuck (%s)\n", strerror(errno));
    exit(1);
  }
}

void client_read(int sockfd, short what, void *priv)
{
  char buf[cfg.client_buf_size];

  struct client *client = priv;

  int n = read(sockfd, buf, cfg.client_buf_size);
  D(printf("client_read: read %d bytes\n", n);)
  if(n==0)
  {
    D(printf("client_read: releasing client %p\n", client);)
    close(sockfd);
    event_del(&client->read_event);
    free(client);
    return;
  }

  int nparsed = http_parser_execute(&client->parser, &client->parser_settings, buf, n);
  if(nparsed != n)
  {
    D(printf("parser failed (parsed %d bytes)\n", nparsed);)
    close(sockfd); /* Automatically deletes kqueue events */
    free(client);
  }
}

void new_con_cb(int sockfd, short what, void *not_used)
{
  D(printf("received new con with socket %d\n", sockfd);)

  struct sockaddr_in saddr;
  memset(&saddr, 0, sizeof(struct sockaddr_in));
  socklen_t addr_len = (socklen_t)sizeof(struct sockaddr_in);
  int accepted_sockfd;

  while( (accepted_sockfd = accept(sockfd, (struct sockaddr*)&saddr, &addr_len)) >= 0)
  {
    D(printf("handling client %s\n", inet_ntoa(saddr.sin_addr));)

    set_nonblocking(accepted_sockfd);
    set_no_tcp_delay(accepted_sockfd);

    struct client *client = (struct client*)malloc(sizeof(struct client));
    memset(client, 0, sizeof(struct client));

    event_set(&client->read_event, accepted_sockfd, EV_READ|EV_PERSIST, client_read, client);

    /* Initialize HTTP parser for this client */
    http_parser_init(&client->parser, HTTP_REQUEST);
  
    client->parser_settings.on_url = zero_on_url;
    client->parser_settings.on_message_complete = zero_on_message_complete;
    client->parser.data = client;
    client->sockfd = accepted_sockfd;

    event_add(&client->read_event, NULL);
  }
  if(errno != EAGAIN)
  {
    fprintf(stderr, "accept failed (%s)?\n", strerror(errno));
    exit(1);
  }
}

int create_listen_socket()
{
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in saddr;
  bzero(&saddr, sizeof(struct sockaddr_in));
  saddr.sin_family = PF_INET;
  saddr.sin_addr.s_addr = htonl(INADDR_ANY);
  saddr.sin_port = htons(cfg.listen_port);

  if(bind(sockfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0)
  {
    fprintf(stderr, "oh no: bind failed\n");
    exit(1);
  } 

  listen(sockfd, cfg.max_pending);

  set_nonblocking(sockfd);

  event_set(&new_con_ev, sockfd, EV_READ|EV_PERSIST, new_con_cb, NULL);
  event_add(&new_con_ev, NULL);

  return 0;
}

void rate_counter_print(int socket, short what, void *not_used)
{
  printf("%d req/s\n", rate_counter_counter);
  rate_counter_counter = 0;

  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  evtimer_add(&rate_counter_event, &tv);
}

void rate_counter_init()
{
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  evtimer_set(&rate_counter_event, rate_counter_print, NULL);
  evtimer_add(&rate_counter_event, &tv);
}

int main()
{
  cfg.listen_port = 8000;
  cfg.max_pending = 5;
  cfg.client_buf_size = 1024;

  event_init();
  create_listen_socket();
  rate_counter_init();
  event_dispatch();
}
