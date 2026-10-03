# swissknife - build with MinGW:  mingw32-make
# CURL must point at a MinGW build of libcurl (needs include/ and lib/).
CC      = gcc
CURL    = C:/curl
CFLAGS  = -O2 -Wall -Wextra -Wno-format-truncation -I$(CURL)/include
LDFLAGS = -L$(CURL)/lib
# static libcurl: add -DCURL_STATICLIB to CFLAGS and list the libs your build needs in EXTRA
EXTRA   =
LIBS    = -lcurl $(EXTRA) -lshlwapi -lole32 -lshell32 -luuid

ifneq ($(MAKECMDGOALS),clean)
TARGET := $(shell $(CC) -dumpmachine)
ifeq (,$(TARGET))
$(error '$(CC)' not found. Install MinGW gcc: pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-curl, then open the "MSYS2 MINGW64" shell, or run: PATH=/mingw64/bin:$$PATH make CURL=/mingw64)
endif
ifeq (,$(findstring mingw,$(TARGET)))
$(error $(CC) builds for '$(TARGET)', not MinGW. In MSYS2 open the "MSYS2 MINGW64" shell (not plain MSYS) and run: pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-curl. Or use Chocolatey's mingw with build.ps1)
endif
ifeq (,$(wildcard $(CURL)/include/curl/curl.h))
$(error curl/curl.h not found under $(CURL)/include. Set CURL= to the folder that contains include/ and lib/, e.g. make CURL=/mingw64)
endif
endif

SRC = sk.c net.c inst.c extract.c cjson.c
HDR = net.h inst.h extract.h config.h cjson.h

sk.exe: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LIBS)

clean:
	-$(RM) sk.exe

.PHONY: clean
