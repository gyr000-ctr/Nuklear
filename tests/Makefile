# Unit tests for optional bidi support and other library features.
# Tests include the amalgamated single-header ../nuklear.h.

CC ?= gcc
CFLAGS += -std=c99 -Wall -Wextra -Wno-misleading-indentation
CFLAGS += -DNK_INCLUDE_FIXED_TYPES -DNK_INCLUDE_STANDARD_IO
CFLAGS += -DNK_INCLUDE_STANDARD_VARARGS -DNK_INCLUDE_DEFAULT_ALLOCATOR
CFLAGS += -DNK_INCLUDE_VERTEX_BUFFER_OUTPUT -DNK_INCLUDE_FONT_BAKING
CFLAGS += -DNK_INCLUDE_DEFAULT_FONT -DNK_INCLUDE_BIDI
LDLIBS += -lm

BIN = bidi_test
IBIN = bidi_integration_test

.PHONY: all clean test integration
all: $(BIN) $(IBIN)

$(BIN): bidi_test.c ../nuklear.h
	$(CC) $(CFLAGS) -I.. -o $@ bidi_test.c $(LDLIBS)

$(IBIN): bidi_integration_test.c ../nuklear.h
	$(CC) $(CFLAGS) -I.. -o $@ bidi_integration_test.c $(LDLIBS)

test: $(BIN) $(IBIN)
	./$(BIN)
	./$(IBIN)

integration: $(IBIN)
	./$(IBIN)

clean:
	rm -f $(BIN) $(IBIN)
