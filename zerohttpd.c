/*
   ZeroHTTP - Bare minimal high performance HTTP Server
   Author   - Daniel Janzon
*/


#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <fcntl.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "http-parser/http_parser.h"

#define D(x) 

struct event; /* Forward declaration */
typedef void (*event_callback)(int sockfd, struct event *ev);

struct event
{
  struct kevent event;
  event_callback callback;
  int sockfd;
  http_parser parser; /* Used to parse HTTP requests */
  http_parser_settings parser_settings;
  char url[1024];
  int fd; /* File that we are currently streaming */
};

/*
 * This is the descriptor for the event set we will
 * be watching in the event loop.
 */
int kevent_base = -1;

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

int serve_file(struct event *ev, const char *filename)
{
  D(printf("serve_file: opening file '%s'\n", filename);)
  ev->fd = open(filename, O_RDONLY);
  if(ev->fd < 0)
  {
    write_header(ev->sockfd, HTTP_404_NOT_FOUND, 0);
    return 0; /* 404 is totally legitimate */
  }

  struct stat sb;
  fstat(ev->fd, &sb);
  write_header(ev->sockfd, HTTP_200_OK, sb.st_size);

  D(printf("serve_file: hdr sent (%d bytes), sending %d bytes of HTTP payload\n", n, sb.st_size);)

  #warning "BUG when sendfile doesn't send all data in one go"
  sendfile(ev->fd, ev->sockfd, 0, 0, NULL, 0, 0);

  close(ev->fd);

  return 0;
}

int zero_on_message_complete(http_parser *parser)
{
  D(printf("got message complete on event %p\n", parser->data);)
  struct event *ev = (struct event*)parser->data;
  char *sp = ev->url;
  while(*sp == '/')
    sp++;

  int err = serve_file(ev, sp);
  rate_counter_counter++;
  return err; /* Hopefully tell parser all went ok */
}

int zero_on_url(http_parser *parser, const char *at, size_t length)
{
  struct event *ev = (struct event*)parser->data;
  D(printf("zero_on_url: got length %d\n", length);)
  length = (length < 1023) ? length : 1023;
  memcpy(ev->url, at, length);
  ev->url[length] = '\0';
  D(printf("got url '%s' on event %p\n", ev->url, ev);)

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

void client_read(int sockfd, struct event *ev)
{
  char buf[cfg.client_buf_size];

  int n = read(sockfd, buf, cfg.client_buf_size);
  D(printf("client_read: read %d bytes\n", n);)
  if(n==0)
  {
    D(printf("client_read: releasing event %p\n", ev);)
    close(sockfd);
    free(ev);
    return;
  }

  int nparsed = http_parser_execute(&ev->parser, &ev->parser_settings, buf, n);
  if(nparsed != n)
  {
    D(printf("parser failed (parsed %d bytes)\n", nparsed);)
    close(sockfd); /* Automatically deletes kqueue events */
    free(ev);
  }
}

void new_con_cb(int sockfd, struct event *not_used)
{
  D(printf("received new con with socket %d\n", sockfd);)

  struct sockaddr_in saddr;
  bzero(&saddr, sizeof(struct sockaddr_in));
  socklen_t addr_len = (socklen_t)sizeof(struct sockaddr_in);
  int accepted_sockfd;

  while( (accepted_sockfd = accept(sockfd, (struct sockaddr*)&saddr, &addr_len)) >= 0)
  {
    D(printf("handling client %s\n", inet_ntoa(saddr.sin_addr));)

    set_nonblocking(accepted_sockfd);
    set_no_tcp_delay(accepted_sockfd);

    struct event *ev = (struct event*)malloc(sizeof(struct event));
    bzero(ev, sizeof(struct event));
    ev->sockfd = accepted_sockfd;
    ev->callback = client_read;

    /* Initialize HTTP parser for this client */
    http_parser_init(&ev->parser, HTTP_REQUEST);
  
    ev->parser_settings.on_url = zero_on_url;
    ev->parser_settings.on_message_complete = zero_on_message_complete;
    ev->parser.data = ev;

    EV_SET(&ev->event, accepted_sockfd, EVFILT_READ, EV_ADD, 0, 0, ev);
    kevent(kevent_base, &ev->event, 1, (void*)0, 0, NULL);
  }
  if(errno != EAGAIN)
  {
    fprintf(stderr, "accept failed (%s)?\n", strerror(errno));
    exit(1);
  }
}

int create_listen_socket()
{
  int sockfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
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

  new_con_ev.sockfd = sockfd;
  new_con_ev.callback = new_con_cb;

  /* Add listen socket to kqueue */
  EV_SET(&new_con_ev.event, sockfd, EVFILT_READ, EV_ADD, 0, 0, &new_con_ev);
  kevent(kevent_base, &new_con_ev.event, 1, (void*)0, 0, NULL);

  return 0;
}

void rate_counter_print(int socket, struct event *ev)
{
  printf("%d req/s\n", rate_counter_counter);
  rate_counter_counter = 0;
}

void rate_counter_init()
{
  rate_counter_event.callback = rate_counter_print;
  EV_SET(&rate_counter_event.event, 0, EVFILT_TIMER, EV_ADD, 0, 1000, &rate_counter_event);
  kevent(kevent_base, &rate_counter_event.event, 1, (void*)0, 0, NULL);
}

void event_loop()
{
  while(1)
  {
    struct kevent events[100];
    int num_events = kevent(kevent_base, (void*)0, 0, events, 100, NULL);
    if(num_events == -1)
      printf("kevent error: %s\n", strerror(errno));

    int i;
    for(i=0; i<num_events; i++)
    {
      struct event *ev = (struct event*)events[i].udata;
      ev->callback(events[i].ident, ev);
      if(events[i].flags & EV_EOF)
      {
        D(printf("got EOF!\n");)
      }
    }
  }
}

int main()
{
  cfg.listen_port = 8000;
  cfg.max_pending = 5;
  cfg.client_buf_size = 1024;

  kevent_base = kqueue();
  create_listen_socket();
  rate_counter_init();
  event_loop();
}
