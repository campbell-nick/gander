# gander - offline previewer for a local git repository
#
# Plain C11, no dependencies beyond libc.

VERSION := 0.1.0
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
SHAREDIR = $(PREFIX)/share/gander
MANDIR   = $(PREFIX)/share/man/man1

CC      ?= cc
CXX     ?= c++
# Optimization/flavour flags, kept separate so a caller (e.g. ./invoke) can
# override them without clobbering the -std/-Wall/-D flags below.
OPT      ?= -O2
CFLAGS  ?= $(OPT)
CFLAGS  += -std=c11 -Wall -Wextra -D_GNU_SOURCE
CXXFLAGS ?= $(OPT)
CXXFLAGS += -std=c++17 -Wall -Wextra -fPIC
LDFLAGS ?=

# Experimental embedded webview. Off by default, so a plain `make` stays
# libc-only. A backend is picked at build time:
#
#   make WEBVIEW=1     auto: WebKitGTK (GTK) if present, else Qt WebEngine
#   make WEBVIEW=gtk   force WebKitGTK    (needs gtk+-3.0 + webkit2gtk-4.1)
#   make WEBVIEW=qt    force Qt WebEngine (needs Qt6WebEngineWidgets)
#
WEBVIEW ?= 0
WV_BACKEND := $(WEBVIEW)

ifeq ($(WEBVIEW),1)
  ifneq ($(shell pkg-config --exists gtk+-3.0 webkit2gtk-4.1 2>/dev/null && echo y),)
    WV_BACKEND := gtk
  else ifneq ($(shell pkg-config --exists Qt6WebEngineWidgets 2>/dev/null && echo y),)
    WV_BACKEND := qt
  else
    $(error WEBVIEW=1 found neither WebKitGTK (gtk+-3.0, webkit2gtk-4.1) nor Qt WebEngine (Qt6WebEngineWidgets); install one, or set WEBVIEW=gtk or WEBVIEW=qt)
  endif
endif

ifeq ($(WV_BACKEND),gtk)
  CFLAGS += -DGANDER_WEBVIEW -DGANDER_WEBVIEW_GTK \
            $(shell pkg-config --cflags gtk+-3.0 webkit2gtk-4.1 2>/dev/null)
  WEBVIEW_LIBS := $(shell pkg-config --libs gtk+-3.0 webkit2gtk-4.1 2>/dev/null)
  ifeq ($(strip $(WEBVIEW_LIBS)),)
    $(error WEBVIEW=gtk needs the gtk+-3.0 and webkit2gtk-4.1 dev packages)
  endif
endif

ifeq ($(WV_BACKEND),qt)
  CFLAGS   += -DGANDER_WEBVIEW -DGANDER_WEBVIEW_QT \
              $(shell pkg-config --cflags Qt6WebEngineWidgets 2>/dev/null)
  CXXFLAGS += -DGANDER_WEBVIEW -DGANDER_WEBVIEW_QT \
              $(shell pkg-config --cflags Qt6WebEngineWidgets 2>/dev/null)
  WEBVIEW_LIBS := $(shell pkg-config --libs Qt6WebEngineWidgets 2>/dev/null)
  ifeq ($(strip $(WEBVIEW_LIBS)),)
    $(error WEBVIEW=qt needs the Qt6 WebEngine Widgets dev package (dev-qt/qtwebengine))
  endif
  WEBVIEW_QT_OBJ := src/webview_qt.o
  LINK := $(CXX)
endif

LINK ?= $(CC)

SRC := src/util.c src/walk.c src/watch.c src/md.c src/html.c src/highlight.c src/repo.c src/server.c src/config.c src/webview.c src/main.c
OBJ := $(SRC:.c=.o) $(WEBVIEW_QT_OBJ)
BIN := gander

.PHONY: all clean install uninstall run check

all: $(BIN)

$(BIN): $(OBJ)
	$(LINK) $(LDFLAGS) -o $@ $(OBJ) $(WEBVIEW_LIBS)

%.o: %.c src/gander.h src/logo.h
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp src/gander.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# The webview glue is compiled with backend-specific flags, so always rebuild it:
# switching WEBVIEW on, off or between backends must not reuse a stale object.
WEBVIEW_GLUE := src/webview.o $(WEBVIEW_QT_OBJ)
$(WEBVIEW_GLUE): FORCE
FORCE:

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
	rm -f src/*.o $(BIN)
