# reference-nodb: the reference's event loop without SQLite. One translation unit, as upstream.
# Needs liburing's ABI header only (see ../bulkapi/Makefile for how to install liburing).
CC       ?= gcc
ARCH     ?= native
LIBURING ?= $(HOME)/.local
CFLAGS   ?= -std=gnu11 -O3 -march=$(ARCH) -fno-plt -fomit-frame-pointer -Wall -Wno-unused-function \
            -Wno-unused-variable -Wno-unused-but-set-variable

build/server: src/server.c src/app.h src/store.h src/http.h src/json.h src/sys.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(LIBURING)/include -Isrc -o $@ src/server.c -lpthread -lm

clean:
	rm -rf build

.PHONY: clean
