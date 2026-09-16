CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -g -O0
LDFLAGS :=

SRC := allocator.c
OBJ := $(SRC:.c=.o)

DEMO_BIN := demo
TEST_BIN := test_allocator

.PHONY: all clean test run-demo

all: $(DEMO_BIN) $(TEST_BIN)

allocator.o: allocator.c allocator.h
	$(CC) $(CFLAGS) -c allocator.c -o allocator.o

$(DEMO_BIN): demo.c allocator.o allocator.h
	$(CC) $(CFLAGS) demo.c allocator.o -o $(DEMO_BIN) $(LDFLAGS)

$(TEST_BIN): tests/test_allocator.c allocator.o allocator.h
	$(CC) $(CFLAGS) tests/test_allocator.c allocator.o -o $(TEST_BIN) $(LDFLAGS)

run-demo: $(DEMO_BIN)
	./$(DEMO_BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(OBJ) $(DEMO_BIN) $(TEST_BIN)
