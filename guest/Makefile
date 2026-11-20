CC = i686-w64-mingw32-gcc
CFLAGS ?= -O2 -Wall -Wextra -s -mcrtdll=msvcrt-os
LDFLAGS ?= -static -mwindows
LIBS ?= -lws2_32 -lgdi32 -luser32 -ladvapi32

all: xpagent.exe

xpagent.exe: xpagent.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f xpagent.exe

.PHONY: all clean
