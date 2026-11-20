CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -s -mcrtdll=msvcrt-os
LDFLAGS = -static -mwindows
LIBS = -lwininet -luser32 -lgdi32 -lshell32

all: xpcode.exe

xpcode.exe: xpcode.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f xpcode.exe

.PHONY: all clean
