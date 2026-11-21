CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= $(HOME)/.local
PLUGINDIR ?= $(PREFIX)/lib/deadbeef
PACKAGES = libcurl jansson glib-2.0
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES))

PLUGIN = build/ddb_listenbrainz.so
OBJECTS = build/plugin.o build/api.o

.PHONY: all install test clean
all: $(PLUGIN)
$(PLUGIN): $(OBJECTS)
	$(CC) $(LDFLAGS) -shared -Wl,-z,defs -o $@ $^ $(LDLIBS)
build/%.o: src/%.c src/api.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<
build:
	mkdir -p $@
install: $(PLUGIN)
	install -Dm755 $(PLUGIN) "$(DESTDIR)$(PLUGINDIR)/ddb_listenbrainz.so"
build/test-host: tests/host.c $(PLUGIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS) -ldl
test: build/test-host
	python3 tests/integration.py
clean:
	rm -f $(PLUGIN) $(OBJECTS) build/test-host
