# Tasks — GTK4 GUI + CLI/TUI
#
#   make            build the GUI (tasks-gui)
#   make test       run the full test suite
#   make install    install for the current user under ~/.local
#   make uninstall  remove the per-user binaries
#   make clean      remove the built binary
#
# For a system-wide install, use the PKGBUILD instead: makepkg -si

CC      ?= gcc
CFLAGS  ?= -O2
WARN     = -Wall -Wextra
PKGS     = gtk4 jansson
BIN      = tasks-gui
SRC      = tasks-gui.c

PREFIX  ?= $(HOME)/.local
BINDIR   = $(PREFIX)/bin
ICONDIR  = $(PREFIX)/share/icons/hicolor/scalable/apps

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

.PHONY: all gui test check install uninstall clean

all: gui

gui: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(WARN) $(PKG_CFLAGS) $(SRC) -o $(BIN) $(PKG_LIBS) -lm

test: gui
	bash tests/run_all.sh

check: test

install: gui
	install -Dm755 $(BIN)            $(DESTDIR)$(BINDIR)/tasks-gui
	install -Dm755 tasks.py          $(DESTDIR)$(BINDIR)/tasks
	install -Dm755 taskstore.py      $(DESTDIR)$(BINDIR)/taskstore.py
	install -Dm755 tasks-setup-user  $(DESTDIR)$(BINDIR)/tasks-setup-user
	install -Dm644 tasks.svg         $(DESTDIR)$(ICONDIR)/org.omarchy.tasks.svg
	@echo "Installed to $(BINDIR)."
	@echo "Now run 'tasks-setup-user' to add the launcher, menu row and keybinding."

uninstall:
	rm -f $(BINDIR)/tasks-gui $(BINDIR)/tasks $(BINDIR)/taskstore.py $(BINDIR)/tasks-setup-user
	@echo "Removed the per-user binaries from $(BINDIR)."

clean:
	rm -f $(BIN)
