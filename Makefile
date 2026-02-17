CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -g
LDFLAGS = -lpthread
BUILD   = build

all: $(BUILD)/test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test: src/hashmap.c src/test.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: $(BUILD)/test
	./$(BUILD)/test

clean:
	rm -rf $(BUILD)

.PHONY: all clean run
