CC ?= cc
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter \
	-Wno-unused-function -Wno-unused-but-set-variable
SAN_FLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
ROOT := ..
BIN := menu_inventory_props
BIN_ASAN := menu_inventory_props_asan
COMMON_ARGS := --cases 200 --frames 12

.PHONY: all test asan clean

all: $(BIN)

$(BIN): test_menu_inventory_props.c $(ROOT)/nuklear.h
	$(CC) $(CFLAGS) -I$(ROOT) -o $@ $< -lm

$(BIN_ASAN): test_menu_inventory_props.c $(ROOT)/nuklear.h
	$(CC) $(SAN_FLAGS) -I$(ROOT) -o $@ $< -lm

test: $(BIN)
	cd $(ROOT) && tests/$(BIN) $(COMMON_ARGS) \
		--frames-out artifacts/property_frames.ndjson \
		--summary-out artifacts/property_summary.json \
		--failures-out artifacts/counterexamples.txt

asan: $(BIN_ASAN)
	cd $(ROOT) && ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		tests/$(BIN_ASAN) $(COMMON_ARGS) --no-frames \
		--summary-out artifacts/property_summary_asan.json \
		--failures-out artifacts/counterexamples_asan.txt

clean:
	rm -f $(BIN) $(BIN_ASAN)
