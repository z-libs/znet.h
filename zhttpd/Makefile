CC = gcc
CFLAGS = -std=c11 -O2 -D_GNU_SOURCE


ifeq ($(OS),Windows_NT)
    TARGETS = zhttpd
    LIBS_STD = -lws2_32
else
    TARGETS = zhttpd zhttpd-async
endif

all: get_dependencies $(TARGETS)

get_dependencies:
	@echo "Using wget to add dependencies..."	
	wget -q "https://raw.githubusercontent.com/z-libs/zerror.h/main/zerror.h"   -O "zerror.h"
	wget -q "https://raw.githubusercontent.com/z-libs/zstr.h/main/zstr.h"       -O "zstr.h"
	wget -q "https://raw.githubusercontent.com/z-libs/zfile.h/main/zfile.h"     -O "zfile.h"
	wget -q "https://raw.githubusercontent.com/z-libs/zthread.h/main/zthread.h" -O "zthread.h"
	cp ../znet.h .

zhttpd: zhttpd_std.c
	$(CC) zhttpd_std.c -o zhttpd $(CFLAGS) $(LIBS_STD)

zhttpd-async: zhttpd_async.c
	$(CC) zhttpd_async.c -o zhttpd-async $(CFLAGS)

bench-std:
	@echo "--- Benchmarking zhttpd (Portable) ---"
	@./zhttpd . 8080 128 & echo $$! > zhttpd.pid
	@sleep 2
	@wrk -t4 -c100 -d5s http://127.0.0.1:8080/big.file
	@kill $$(cat zhttpd.pid) || rm zhttpd.pid

bench-async:
	@echo "--- Benchmarking zhttpd-async (Performance) ---"
	@ulimit -n 100000
	@./zhttpd-async . & echo $$! > zasync.pid
	@sleep 1
	@taskset -c 0-5 wrk -t6 -c1000 -d10s http://127.0.0.1:8080/big.file
	@pkill zhttpd-async || rm zasync.pid

clean:
	@echo "Removing dependencies..."
	@rm -f zerror.h zstr.h zfile.h zthread.h znet.h
	@echo "Removing binaries..."
	@rm -f zhttpd zhttpd-async *.o *.pid

.PHONY: all get_dependencies clean bench-std bench-async
