# Accessibility layer test suite and regression baseline
CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -g -O0
ROOT := ../..
SINGLE_HEADER := $(ROOT)/nuklear.h
BASELINE := baseline/snapshot_baseline.json

.PHONY: all test baseline-check clean

all: test_a11y baseline_gen

$(SINGLE_HEADER):
	$(MAKE) -C $(ROOT) nuke

test_a11y: test_a11y.c $(SINGLE_HEADER)
	$(CC) $(CFLAGS) -I$(ROOT) -o $@ test_a11y.c -lm

baseline_gen: baseline_gen.c $(SINGLE_HEADER)
	$(CC) $(CFLAGS) -I$(ROOT) -o $@ baseline_gen.c -lm

# functional + memory tests, then byte-for-byte snapshot regression
test: test_a11y baseline_gen
	./test_a11y
	./baseline_gen > snapshot.generated.json
	diff -u $(BASELINE) snapshot.generated.json
	@echo "baseline regression: OK"
	rm -f snapshot.generated.json

# regenerate the checked-in baseline after an intentional tree change
baseline-check: baseline_gen
	./baseline_gen > $(BASELINE)

clean:
	rm -f test_a11y baseline_gen
