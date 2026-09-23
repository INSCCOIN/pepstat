CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
LDFLAGS ?=
PREFIX  ?= $(HOME)
BINDIR  ?= $(PREFIX)/bin

.PHONY: all clean install

all: pepstat

pepstat: pepstat.c
	$(CC) $(CFLAGS) -o $@ pepstat.c $(LDFLAGS)

install: pepstat
	mkdir -p $(BINDIR)
	cp pepstat $(BINDIR)/pepstat

clean:
	rm -f pepstat
