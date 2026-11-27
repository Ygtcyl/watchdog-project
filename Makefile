CC       ?= gcc
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS += -D_XOPEN_SOURCE=700 -Iinclude

BUILD    := build
SRCS     := src/main.c src/signal_manager.c src/process_manager.c src/logger.c
OBJS     := $(SRCS:src/%.c=$(BUILD)/%.o)
DEPS     := $(OBJS:.o=.d)

.PHONY: all clean test

all: watchdog worker

watchdog: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

worker: workers/worker.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $@

test: all
	bash tests/test_scenarios.sh

clean:
	rm -rf $(BUILD) watchdog worker watchdog.log tests/logs

-include $(DEPS)
