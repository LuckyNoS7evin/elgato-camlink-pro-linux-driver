CFLAGS ?= -O2 -Wall -Wextra

framer_test: framer_test.c ../driver/clp-framer.c ../driver/clp-framer.h
	$(CC) $(CFLAGS) -o $@ framer_test.c

clean:
	rm -f framer_test
