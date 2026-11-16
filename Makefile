# gander - offline previewer for a local folder or git repository
#
# Plain C11, no dependencies beyond libc.

VERSION := 0.1.0
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
SHAREDIR = $(PREFIX)/share/gander

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS ?=

SRC := src/util.c src/walk.c src/watch.c src/md.c src/server.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := gander

.PHONY: all clean install uninstall run

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c src/gander.h
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN) .

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(SHAREDIR)/web
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -m 0644 web/* $(DESTDIR)$(SHAREDIR)/web/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -rf $(DESTDIR)$(SHAREDIR)

clean:
	rm -f $(OBJ) $(BIN)
