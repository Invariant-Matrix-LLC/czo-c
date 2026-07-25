CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wstrict-prototypes -O2
PREFIX  ?= /usr/local

OBJ = czo.o main.o

all: czo

czo: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

czo.o: czo.c czo.h
	$(CC) $(CFLAGS) -c -o $@ czo.c

main.o: main.c czo.h
	$(CC) $(CFLAGS) -c -o $@ main.c

# static library, for embedding
libczo.a: czo.o
	ar rcs $@ czo.o

test: czo
	./czo --selftest

install: czo
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 czo $(DESTDIR)$(PREFIX)/bin/czo

clean:
	rm -f $(OBJ) czo libczo.a fuzz

.PHONY: all test install clean

# randomized memory-safety + idempotence harness (ASan/UBSan)
fuzz: fuzz.c czo.c czo.h
	$(CC) -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
	      -o fuzz fuzz.c czo.c
	./fuzz

.PHONY: fuzz
