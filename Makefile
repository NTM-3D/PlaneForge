VERSION := 0.1

ifeq ($(OS),Windows_NT)
  BIN   := PlaneForge_v$(VERSION).exe
  RCOBJ := src/version_rc.o
else
  BIN   := planeforge
  RCOBJ :=
endif

RM := rm -f

CC     ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
SRCS   := src/annexb.c src/ofmd.c src/ofs.c src/mvc.c src/progress.c src/main.c
OBJS   := $(SRCS:.c=.o)

all: $(BIN)

$(BIN): $(OBJS) $(RCOBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(RCOBJ)

src/%.o: src/%.c src/pf.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/version_rc.o: src/version.rc
	windres src/version.rc -O coff -o $@

clean:
	-$(RM) $(OBJS) $(RCOBJ) $(BIN) planeforge.exe planeforge

.PHONY: all clean
