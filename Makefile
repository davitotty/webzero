CC = gcc
CPPFLAGS = -I. -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror
CORE = core/pool.c core/bundle.c core/router.c core/vm.c core/http.c core/connection.c
HEADERS = $(wildcard core/*.h platform/*.h)
all: webzero
webzero: main.c $(CORE) platform/linux.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ main.c $(CORE) platform/linux.c
static: main.c $(CORE) platform/linux.c $(HEADERS)
	musl-gcc $(CPPFLAGS) $(CFLAGS) -static -o webzero-static main.c $(CORE) platform/linux.c
windows: main.c $(CORE) platform/windows.c $(HEADERS)
	$(CC_WIN) -I. $(CFLAGS) -D_WIN32_WINNT=0x0501 -o webzero.exe main.c $(CORE) platform/windows.c -lws2_32 -lkernel32
CC_WIN = i686-w64-mingw32-gcc
debug: main.c $(CORE) platform/linux.c $(HEADERS)
	$(CC) $(CPPFLAGS) -std=c99 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -o webzero-debug main.c $(CORE) platform/linux.c
unit: tests/unit.c core/http.c core/vm.c core/pool.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o tests/unit tests/unit.c core/http.c core/vm.c core/pool.c
	./tests/unit
test: all unit
	python3 tests/native.py ./webzero
	python3 tests/native_vm.py ./webzero
wzimg: tools/wzimg.c third_party/stb_image.h third_party/stb_image_write.h
	$(CC) -std=c99 -O2 -Wall -Wextra -o $@ tools/wzimg.c -lm
clean:
	rm -f webzero webzero-static webzero-debug webzero.exe tests/unit tests/unit-asan wzimg *.o
.PHONY: all static windows debug unit test clean
