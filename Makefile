

all:
	gcc -g -Wall -std=gnu99 -o zerohttpd zerohttpd.c http-parser/http_parser.c -levent

clean:
	rm *.o zerohttpd

