VERSION ?= 0.1.0
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
UNITDIR ?= /etc/systemd/system

CC ?= cc
CURL_CFLAGS ?= $(shell curl-config --cflags 2>/dev/null)
CURL_LIBS ?= $(shell curl-config --libs 2>/dev/null || echo -lcurl)

WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wvla
CPPFLAGS += -D_DEFAULT_SOURCE -DLP_VERSION=\"$(VERSION)\" -DCJSON_NESTING_LIMIT=16 -Iinclude -Ithird_party/cjson
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 $(WARNINGS)
HARDEN = -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
LDLIBS += $(CURL_LIBS) -lm

SRC = src/collector_linux.c src/config.c src/log.c src/options.c src/policy.c \
      src/procfs.c src/protocol.c src/state.c src/transport.c src/util.c
CJSON = third_party/cjson/cJSON.c
CORE_SRC = src/config.c src/log.c src/options.c src/policy.c src/procfs.c src/protocol.c src/state.c src/util.c

BUILD = build
BIN = $(BUILD)/livepulser-agent
TEST_BIN = $(BUILD)/unit-tests
TESTING_BIN = $(BUILD)/livepulser-agent-testing
SANITIZE = -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all clean test test-unit test-integration install uninstall

all: $(BIN)

$(BUILD):
	mkdir -p $@

$(BUILD)/cJSON.o: $(CJSON) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -c $< -o $@

$(BIN): $(SRC) src/main.c $(BUILD)/cJSON.o include/*.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CURL_CFLAGS) $(CFLAGS) $(HARDEN) -Werror $(SRC) src/main.c $(BUILD)/cJSON.o -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD)/cJSON-sanitize.o: $(CJSON) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZE) -w -c $< -o $@

$(TEST_BIN): $(CORE_SRC) tests/unit.c $(BUILD)/cJSON-sanitize.o include/*.h | $(BUILD)
	$(CC) $(CPPFLAGS) -DLP_TESTING $(CFLAGS) $(SANITIZE) -Werror $(CORE_SRC) tests/unit.c $(BUILD)/cJSON-sanitize.o -o $@ -lm

$(TESTING_BIN): $(SRC) src/main.c $(BUILD)/cJSON-sanitize.o include/*.h | $(BUILD)
	$(CC) $(CPPFLAGS) -DLP_TESTING $(CURL_CFLAGS) $(CFLAGS) $(SANITIZE) -Werror $(SRC) src/main.c $(BUILD)/cJSON-sanitize.o -o $@ $(LDFLAGS) $(LDLIBS)

test: test-unit test-integration

test-unit: $(TEST_BIN)
	cd tests && ../$(TEST_BIN)

test-integration: $(TESTING_BIN)
	python3 tests/integration/run.py $(TESTING_BIN)

install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/livepulser-agent
	install -D -m 0644 packaging/systemd/livepulser-agent.service $(DESTDIR)$(UNITDIR)/livepulser-agent.service
	install -d -m 0750 $(DESTDIR)$(SYSCONFDIR)/livepulser-agent
	test -e $(DESTDIR)$(SYSCONFDIR)/livepulser-agent/agent.conf || \
		install -m 0640 packaging/agent.conf.example $(DESTDIR)$(SYSCONFDIR)/livepulser-agent/agent.conf

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/livepulser-agent $(DESTDIR)$(UNITDIR)/livepulser-agent.service

clean:
	rm -rf $(BUILD)
