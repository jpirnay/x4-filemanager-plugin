# Build the CrossPoint/MicroReader diagnostic tool.
#
#   make                  # native build (Linux: diagnose; Windows MSYS2: diagnose.exe)
#   make clean
#
# On Windows (MSYS2 MinGW64 shell):
#   pacman -S mingw-w64-x86_64-gcc make
#   make

SRC    := diagnose.c ../protocol/cp_serial.c
HDR    := ../protocol/cp_serial.h
CFLAGS := -O2 -Wall -Wno-format

ifdef SYSTEMROOT
  OUT := diagnose.exe
  LIBS := -lsetupapi
else
  OUT := diagnose
  LIBS :=
endif

.PHONY: all clean

all: $(OUT)
$(OUT): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS)

clean:
	rm -f diagnose diagnose.exe diagnose_download.tmp
