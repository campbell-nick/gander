# gander - offline previewer for a local git repository
#
# Plain C11, no dependencies beyond libc.

VERSION := 0.2.0
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
SHAREDIR = $(PREFIX)/share/gander
MANDIR   = $(PREFIX)/share/man/man1

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS ?=

# Live-reload backend. Linux defaults to inotify; every other system falls back
# to a portable polling scan. Override to force one (mainly to exercise the
# portable path on Linux):
#
#   make WATCH=auto      inotify on Linux, polling elsewhere (default)
#   make WATCH=inotify   force inotify     (Linux only)
#   make WATCH=poll      force the portable polling scan
#
WATCH ?= auto
ifeq ($(WATCH),poll)
  CFLAGS += -DGANDER_WATCH_POLL
else ifeq ($(WATCH),inotify)
  CFLAGS += -DGANDER_WATCH_INOTIFY
else ifneq ($(WATCH),auto)
  $(error WATCH must be auto, inotify or poll)
endif

SRC := src/util.c src/walk.c src/watch.c src/md.c src/html.c src/highlight.c src/repo.c src/server.c src/config.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := gander
CONFIG := config.example

.PHONY: all clean install uninstall run check

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

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
	install -m 0644 $(CONFIG) $(DESTDIR)$(SHAREDIR)/config.example
	@if [ -z "$(DESTDIR)" ]; then \
		if [ -n "$$XDG_CONFIG_HOME" ]; then cfgdir=$$XDG_CONFIG_HOME; \
		elif [ -n "$$HOME" ]; then cfgdir=$$HOME/.config; \
		else cfgdir=; fi; \
		if [ -n "$$cfgdir" ] && [ -z "$$GANDER_CONFIG" ] && \
		   [ ! -f "$$cfgdir/gander/config" ]; then \
			mkdir -p "$$cfgdir/gander"; \
			install -m 0644 $(CONFIG) "$$cfgdir/gander/config"; \
			printf 'gander: created %s/gander/config\n' "$$cfgdir"; \
		fi; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -rf $(DESTDIR)$(SHAREDIR)
	rm -f $(DESTDIR)$(MANDIR)/gander.1

clean:
	rm -f src/*.o $(BIN)
