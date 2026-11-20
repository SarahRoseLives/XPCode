CC = i686-w64-mingw32-gcc
RC = i686-w64-mingw32-windres
CFLAGS = -O2 -Wall -Wextra -s -mcrtdll=msvcrt-os
LDFLAGS = -static -mwindows
LIBS = -lwininet -luser32 -lgdi32 -lshell32 -lole32

all: xpcode.exe

xpcode.exe: xpcode.c xpcode.rc resource.h
	$(RC) xpcode.rc -O coff -o xpcode.res
	$(CC) $(CFLAGS) -o $@ xpcode.c xpcode.res $(LDFLAGS) $(LIBS)

clean:
	rm -f xpcode.exe xpcode.res

.PHONY: all clean
