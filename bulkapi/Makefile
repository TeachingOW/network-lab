CXX      ?= g++
ARCH     ?= native
# -flto keeps cross-file inlining (process(), handlers, store) now that the
# server is split into several translation units.
CXXFLAGS ?= -O3 -march=$(ARCH) -std=c++20 -Wall -Wextra -pthread -fno-plt -flto=auto

SERVER_SRCS := main backend_uring backend_epoll net http conn handlers json store
SERVER_OBJS := $(SERVER_SRCS:%=build/%.o)

all: build/server build/bench

# The route spec is "fed in" here, before the server is compiled.
build/routes_gen.h: api.spec tools/gen_routes.py
	@mkdir -p build
	python3 tools/gen_routes.py api.spec $@

# Every object needs the generated router; -MMD tracks the other headers.
build/%.o: src/%.cpp build/routes_gen.h
	$(CXX) $(CXXFLAGS) -MMD -MP -Ibuild -c -o $@ $<

build/server: $(SERVER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(SERVER_OBJS)

build/bench: bench/bench.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ bench/bench.cpp

clean:
	rm -rf build

-include $(SERVER_OBJS:.o=.d)

.PHONY: all clean
