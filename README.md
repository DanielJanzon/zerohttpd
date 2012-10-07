zerohttpd
=========

The simplest (but no simpler) high performance HTTP server possible just in order to try out some FreeBSD programming

ZeroHTTP achieves high performance by

  (1) Using the kqueue interface which makes the event handling
      system call O(m) instead of O(n) when there are m sockets
      out of n that has had an event.

  (2) Using the sendfile system call that takes the content of
      a file and sends it out on a socket without having to
      read the file into user space memory first.

  (3) Setting the TCP_NODELAY option on its sockets. Without
      this tuning, ZeroHTTP can only handle 10 req/s since the
      kevent call that waits for new events takes around 100 ms
      to return.

  (4) No calls to malloc are performed in fast path, only when
      new sockets connect.

  (5) Use somebody else's fast HTTP parser

  (6) [Not implemented] Cache file descriptors in order to save
      open/fstat/close cycle.

Performance can be measured with the httperf tool, i.e. by running

 httperf --hog --port 8000 --server 127.0.0.1 --num-conns=10 \
         --num-calls=7000 --uri /myfile


Limits, bugs and known issues
=============================

  (1) ZeroHTTP is only HTTP/1.1 compatible.

  (2) ZeroHTTP serves only files in its own current directory.

