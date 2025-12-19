
all: get_dependencies compile

get_dependencies:
	@echo "Using wget to add dependencies..."	
	wget -q "https://raw.githubusercontent.com/z-libs/zerror.h/main/zerror.h"   -O "zerror.h"
	wget -q "https://raw.githubusercontent.com/z-libs/zstr.h/main/zstr.h"       -O "zstr.h"
	wget -q "https://raw.githubusercontent.com/z-libs/zfile.h/main/zfile.h"     -O "zfile.h"
	wget -q "https://raw.githubusercontent.com/z-libs/zthread.h/main/zthread.h" -O "zthread.h"
	cp ../znet.h .

compile:
	gcc zhttpd.c -o zhttpd -std=c11 -O2

clean:
	@echo "Removing dependencies..."
	@rm zerror.h zstr.h zfile.h zthread.h znet.h
	@echo "Removing binary..."
	@rm zhttpd

.PHONY: get_dependencies clean
