CC ?= gcc
CFLAGS ?= -Ofast -flto -fno-math-errno -fomit-frame-pointer -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE -pthread
LDFLAGS ?= -flto -lm -pthread
LB_LDFLAGS ?= -flto -pthread

BIN := build/rinha-api
LB := build/rinha-lb
INDEX_BUILDER := build/build-index
SRC := src/main.c
LB_SRC ?= src/lb-fd.c
INDEX_BUILDER_SRC := src/build-index.c

.PHONY: all clean run

all: $(BIN) $(LB) $(INDEX_BUILDER)

$(BIN): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(LB): $(LB_SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(LB_SRC) $(LB_LDFLAGS)

$(INDEX_BUILDER): $(INDEX_BUILDER_SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(INDEX_BUILDER_SRC) $(LDFLAGS)

clean:
	rm -rf build
