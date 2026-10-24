CC ?= gcc
CFLAGS ?= -O2 -Wall -pthread
PREFIX ?= $(HOME)/.local

all: camstream camstream_videoonly

camstream: camstream.c
	$(CC) $(CFLAGS) -o $@ $< -lasound

camstream_videoonly: camstream_videoonly.c
	$(CC) $(CFLAGS) -o $@ $<

install: camstream
	install -d $(PREFIX)/bin
	install -m 755 camstream $(PREFIX)/bin/camstream

clean:
	rm -f camstream camstream_videoonly *.o

.PHONY: all clean install
