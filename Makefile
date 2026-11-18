# gander - offline previewer for a local git repository
#
# Plain C11, no dependencies beyond libc.

VERSION := 0.1.0
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
SHAREDIR = $(PREFIX)/share/gander
MANDIR   = $(PREFIX)/share/man/man1

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS ?=

# Experimental embedded webview (WebKitGTK on GTK3). Off by default, so a plain
# `make` stays libc-only. Enable it with: make WEBVIEW=1
WEBVIEW ?= 0
ifeq ($(WEBVIEW),1)
  WEBVIEW_CFLAGS := -DGANDER_WEBVIEW $(shell pkg-config --cflags gtk+-3.0 webkit2gtk-4.1 2>/dev/null)
  WEBVIEW_LIBS   := $(shell pkg-config --libs gtk+-3.0 webkit2gtk-4.1 2>/dev/null)
  ifeq ($(strip $(WEBVIEW_LIBS)),)
    $(error WEBVIEW=1 needs the gtk+-3.0 and webkit2gtk-4.1 dev packages; pkg-config could not find them)
  endif
  CFLAGS += $(WEBVIEW_CFLAGS)
endif

SRC := src/util.c src/walk.c src/watch.c src/md.c src/html.c src/highlight.c src/repo.c src/server.c src/config.c src/webview.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := gander

.PHONY: all clean install uninstall run check

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(WEBVIEW_LIBS)

%.o: %.c src/gander.h src/logo.h
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN) .

check: $(BIN)
	./tests/run.sh

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(SHAREDIR)/web $(DESTDIR)$(MANDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -m 0644 web/* $(DESTDIR)$(SHAREDIR)/web/
	install -m 0644 man/gander.1 $(DESTDIR)$(MANDIR)/gander.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -rf $(DESTDIR)$(SHAREDIR)
	rm -f $(DESTDIR)$(MANDIR)/gander.1

clean:
	rm -f $(OBJ) $(BIN)
