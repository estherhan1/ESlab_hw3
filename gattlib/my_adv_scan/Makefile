# Build against the installed GATTLIB (sudo make install in gattlib-src/build)
CFLAGS  += -Wall -Wextra -O2 $(shell pkg-config --cflags gattlib)
LDLIBS  += $(shell pkg-config --libs gattlib) -lpthread

my_adv_scan: my_adv_scan.c

clean:
	rm -f my_adv_scan

.PHONY: clean
