# The lightweight parquet reader.
#
# The only dependencies are a C99 compiler and libzstd. The reader decompresses
# zstd pages, and libzstd is the reference implementation of that codec, in C.
CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wpedantic -Isrc
LDLIBS  ?= -lzstd

# Homebrew keeps its headers and libraries outside the default search path.
BREW    := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW),)
CFLAGS  += -I$(BREW)/include
LDFLAGS += -L$(BREW)/lib
endif

BUILD = build
SRC   = src/thrift.c src/meta.c src/rle.c src/parquet.c
HDR   = $(wildcard src/*.h)

TEST_SRC = test/run_tests.c test/check.c test/test_thrift.c test/test_rle.c \
           test/test_parquet.c
TEST_DEP = $(SRC)

.DEFAULT_GOAL := all

all: $(BUILD)/run-tests $(BUILD)/dump-meta $(BUILD)/dump-strings \
      $(BUILD)/peek-strings

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/run-tests: $(TEST_SRC) $(TEST_DEP) $(HDR) test/check.h test/suites.h \
                    | $(BUILD)
	$(CC) $(CFLAGS) -Itest -o $@ $(TEST_SRC) $(TEST_DEP) $(LDFLAGS) $(LDLIBS)

$(BUILD)/dump-meta: tools/dump_meta.c $(SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/dump_meta.c $(SRC) $(LDFLAGS) $(LDLIBS)

$(BUILD)/dump-strings: tools/dump_strings.c $(SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/dump_strings.c $(SRC) $(LDFLAGS) $(LDLIBS)

$(BUILD)/peek-strings: tools/peek_strings.c $(SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/peek_strings.c $(SRC) $(LDFLAGS) $(LDLIBS)

test: $(BUILD)/run-tests
	$(BUILD)/run-tests

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
