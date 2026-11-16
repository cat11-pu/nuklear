CC ?= cc
CFLAGS ?= -O3 -DNDEBUG -std=c99 -Wall -Wextra
BASELINE_REV ?= HEAD
FRAMES ?= 500
ROOT := ..
BUILD := build

.PHONY: all clean baseline optimized compare verify

all: optimized baseline

$(BUILD):
	mkdir -p $(BUILD)/baseline $(BUILD)/optimized

$(BUILD)/baseline/nuklear.h: | $(BUILD)
	git -C $(ROOT) show $(BASELINE_REV):nuklear.h > $@

$(BUILD)/optimized/nuklear.h: $(ROOT)/nuklear.h | $(BUILD)
	cp $< $@

$(BUILD)/bench-baseline: bench.c $(BUILD)/baseline/nuklear.h
	$(CC) $(CFLAGS) -I$(BUILD)/baseline -o $@ bench.c -lm

$(BUILD)/bench-optimized: bench.c $(BUILD)/optimized/nuklear.h
	$(CC) $(CFLAGS) -I$(BUILD)/optimized -o $@ bench.c -lm

baseline: $(BUILD)/bench-baseline

optimized: $(BUILD)/bench-optimized

compare: all
	./$(BUILD)/bench-baseline 32 3 $(FRAMES) $(BUILD)/baseline-32.rgba
	./$(BUILD)/bench-optimized 32 3 $(FRAMES) $(BUILD)/optimized-32.rgba
	./$(BUILD)/bench-baseline 64 3 $(FRAMES) $(BUILD)/baseline-64.rgba
	./$(BUILD)/bench-optimized 64 3 $(FRAMES) $(BUILD)/optimized-64.rgba
	./$(BUILD)/bench-baseline 128 3 $(FRAMES) $(BUILD)/baseline-128.rgba
	./$(BUILD)/bench-optimized 128 3 $(FRAMES) $(BUILD)/optimized-128.rgba
	cmp $(BUILD)/baseline-32.rgba $(BUILD)/optimized-32.rgba
	cmp $(BUILD)/baseline-64.rgba $(BUILD)/optimized-64.rgba
	cmp $(BUILD)/baseline-128.rgba $(BUILD)/optimized-128.rgba
	@echo "32/64/128 windows: pixel-identical"

verify: all
	./$(BUILD)/bench-baseline 64 3 200 $(BUILD)/long-path-baseline.rgba 1
	./$(BUILD)/bench-optimized 64 3 200 $(BUILD)/long-path-optimized.rgba 1
	cmp $(BUILD)/long-path-baseline.rgba $(BUILD)/long-path-optimized.rgba
	$(CC) $(CFLAGS) -DBENCH_FLOAT_COLOR -I$(BUILD)/baseline -o $(BUILD)/bench-baseline-float bench.c -lm
	$(CC) $(CFLAGS) -DBENCH_FLOAT_COLOR -I$(BUILD)/optimized -o $(BUILD)/bench-optimized-float bench.c -lm
	./$(BUILD)/bench-baseline-float 64 3 200 $(BUILD)/float-baseline.rgba
	./$(BUILD)/bench-optimized-float 64 3 200 $(BUILD)/float-optimized.rgba
	cmp $(BUILD)/float-baseline.rgba $(BUILD)/float-optimized.rgba
	$(CC) $(CFLAGS) -DBENCH_GENERIC_LAYOUT -I$(BUILD)/baseline -o $(BUILD)/bench-baseline-generic bench.c -lm
	$(CC) $(CFLAGS) -DBENCH_GENERIC_LAYOUT -I$(BUILD)/optimized -o $(BUILD)/bench-optimized-generic bench.c -lm
	./$(BUILD)/bench-baseline-generic 64 3 200 $(BUILD)/generic-baseline.rgba
	./$(BUILD)/bench-optimized-generic 64 3 200 $(BUILD)/generic-optimized.rgba
	cmp $(BUILD)/generic-baseline.rgba $(BUILD)/generic-optimized.rgba
	@echo "long-path/float-color/generic-layout: pixel-identical"

clean:
	rm -rf $(BUILD)
