# bulkapi-java: needs a JDK 11+ and liburing 2.3+ (see ../bulkapi/Makefile for how to build liburing).
JAVA_HOME ?= $(shell dirname $(shell dirname $(shell readlink -f $(shell which javac))))
LIBURING  ?= $(HOME)/.local
CC        ?= gcc
SRCS      := $(wildcard src/bulkapi/*.java)

all: build/classes/bulkapi/Server.class build/libbulkuring.so

build/classes/bulkapi/Server.class: $(SRCS)
	@mkdir -p build/classes
	javac -Xlint:all -d build/classes $(SRCS)

# The shim links liburing dynamically; the rpath lets the JVM find it without LD_LIBRARY_PATH.
build/libbulkuring.so: native/uring_jni.c
	@mkdir -p build
	$(CC) -O2 -Wall -Wextra -fPIC -shared -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux \
	    -I$(LIBURING)/include -o $@ native/uring_jni.c -L$(LIBURING)/lib -luring -Wl,-rpath,$(LIBURING)/lib

clean:
	rm -rf build

.PHONY: all clean
