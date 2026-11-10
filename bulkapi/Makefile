CXX      ?= g++
ARCH     ?= native
# -flto keeps cross-file inlining (process(), handlers, store) now that the
# server is split into several translation units.
CXXFLAGS ?= -O3 -march=$(ARCH) -std=c++20 -Wall -Wextra -pthread -fno-plt -flto=auto

# liburing 2.3 or newer (for io_uring_prep_recv_multishot). Ubuntu 22.04 packages 2.1, which is
# too old, so build a current release and point LIBURING at its install prefix:
#   git clone --depth 1 --branch liburing-2.15 https://github.com/axboe/liburing.git
#   cd liburing && ./configure --prefix=$$HOME/.local && make -C src && make -C src install
LIBURING ?= $(HOME)/.local
LIBURING_A := $(LIBURING)/lib/liburing.a

SERVER_SRCS := main backend_uring backend_uring_ms backend_epoll net http conn handlers json store
SERVER_OBJS := $(SERVER_SRCS:%=build/%.o)

all: build/server build/bench

# The route spec is "fed in" here, before the server is compiled.
build/routes_gen.h: api.spec tools/gen_routes.py
	@mkdir -p build
	python3 tools/gen_routes.py api.spec $@

# Every object needs the generated router; -MMD tracks the other headers.
build/%.o: src/%.cpp build/routes_gen.h
	$(CXX) $(CXXFLAGS) -MMD -MP -Ibuild -I$(LIBURING)/include -c -o $@ $<

build/server: $(SERVER_OBJS) $(LIBURING_A)
	$(CXX) $(CXXFLAGS) -o $@ $(SERVER_OBJS) $(LIBURING_A)

$(LIBURING_A):
	@echo "liburing not found at $(LIBURING); see the note at the top of this Makefile" >&2; exit 1

build/bench: bench/bench.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ bench/bench.cpp

clean:
	rm -rf build

-include $(SERVER_OBJS:.o=.d)

.PHONY: all clean
