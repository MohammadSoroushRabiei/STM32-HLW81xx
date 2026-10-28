# Host-side tests: gcc + simulated chip. No hardware, no HAL needed.
CC      ?= gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -O1 -g \
           -DHLW8110_HAL_HEADER=\"hal_stub.h\" -I../include -Istubs
test: test_hlw8110
	./test_hlw8110
test_hlw8110: test_hlw8110.c ../src/hlw8110.c
	$(CC) $(CFLAGS) $^ -lm -o $@
clean:
	rm -f test_hlw8110
.PHONY: test clean
