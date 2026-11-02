PREFIX ?= $(HOME)/.local
DESTDIR ?=
CFLAGS ?= -O2
CFLAGS += -Wall
# Where Time Doctor's Linux installer puts the app; only the menu icon is read from it here.
TIMEDOCTOR_DIR ?= $(HOME)/timedoctor
ICON ?= $(TIMEDOCTOR_DIR)/timedoctor.png

LIBDIR = $(PREFIX)/lib/td-waylandshot
INSTALLDIR = $(DESTDIR)$(LIBDIR)
APPLICATIONSDIR = $(DESTDIR)$(PREFIX)/share/applications
SHIM_SOURCES = td-waylandshot.c td-activewindow.c td-sessiontype.c td-microphone.c
INSTALLED_FILES = td-waylandshot.so td-inputbridge timedoctor-wayland

# Copy beside the target and rename over it, so a running Time Doctor keeps the
# file it mapped. An unchanged file is left alone: replacing td-inputbridge
# drops the setgid bit it needs.
place = [ ! -L $(2) ] && cmp -s $(1) $(2) || { install -m$(3) $(1) $(2).new && mv -f $(2).new $(2); }

.PHONY: all install uninstall test clean

all: td-waylandshot.so td-inputbridge

td-waylandshot.so: $(SHIM_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC $(LDFLAGS) -o $@ $(SHIM_SOURCES) -ldl -lxcb -lxcb-image -lpthread

td-inputbridge: td-inputbridge.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< -lX11 -lXtst

install: all
	mkdir -p $(INSTALLDIR) $(APPLICATIONSDIR)
	$(foreach file,$(INSTALLED_FILES),$(call place,$(file),$(INSTALLDIR)/$(file),755);)
	sed -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@ICON@|$(ICON)|' timedoctor.desktop.in > $(APPLICATIONSDIR)/timedoctor.desktop
	@# Setgid input reads every keyboard, so only the owner may run it (2750, not
	@# 2755): another account could otherwise point DISPLAY at its own X server
	@# and receive the timing of every key press.
	@if [ -z "$(DESTDIR)" ] && [ "$$(stat -c %G:%a $(INSTALLDIR)/td-inputbridge)" != input:2750 ]; then \
		echo "td-inputbridge needs the input group to read /dev/input; run once:" >&2; \
		echo "  sudo chgrp input $(INSTALLDIR)/td-inputbridge && sudo chmod 2750 $(INSTALLDIR)/td-inputbridge" >&2; \
	fi

uninstall:
	rm -f $(addprefix $(INSTALLDIR)/,$(INSTALLED_FILES))
	rmdir --ignore-fail-on-non-empty $(INSTALLDIR) 2>/dev/null || true
	rm -f $(APPLICATIONSDIR)/timedoctor.desktop

test: all
	tests/install.test.sh

clean:
	rm -f td-waylandshot.so td-inputbridge
