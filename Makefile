VERSION ?= 0.1.0
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CC ?= cc
TARGET ?= $(if $(findstring mingw,$(CC)),windows,$(shell uname -s | tr A-Z a-z))

WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wvla
CPPFLAGS += -D_DEFAULT_SOURCE -DLP_VERSION=\"$(VERSION)\" -DCJSON_NESTING_LIMIT=16 -Iinclude -Ithird_party/cjson
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 $(WARNINGS) $(ARCHFLAGS)
LDFLAGS += $(ARCHFLAGS)
HARDEN = -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
SANITIZE = -fsanitize=address,undefined -fno-omit-frame-pointer

CORE = src/config.c src/log.c src/metrics.c src/options.c src/policy.c src/procfs.c src/protocol.c src/state.c src/util.c
AGENT = src/collector.c src/main.c

ifeq ($(TARGET),windows)
EXE = .exe
PLATFORM = src/platform_windows.c
OS = src/os_windows.c src/service_windows.c src/transport_winhttp.c
CPPFLAGS += -D_WIN32_WINNT=0x0A00 -D__USE_MINGW_ANSI_STDIO=1
LDFLAGS += -static
LDLIBS += -lwinhttp -liphlpapi -lpdh -lbcrypt -lws2_32 -ladvapi32
HARDEN = -fstack-protector-strong
SANITIZE =
TEST_LIBS = -lbcrypt -ladvapi32
else
PLATFORM = src/platform_posix.c
CURL_CFLAGS ?= $(shell curl-config --cflags 2>/dev/null)
CURL_LIBS ?= $(shell curl-config --libs 2>/dev/null || echo -lcurl)
CPPFLAGS += $(CURL_CFLAGS)
LDLIBS += $(CURL_LIBS) -lm
ifeq ($(TARGET),darwin)
OS = src/os_macos.c src/service_posix.c src/transport_curl.c
CFLAGS += -mmacosx-version-min=11.0
else
OS = src/os_linux.c src/service_posix.c src/transport_curl.c
endif
TEST_LIBS = -lm
endif

CJSON = third_party/cjson/cJSON.c
SRC = $(CORE) $(PLATFORM) $(OS) $(AGENT)
BUILD = build
BIN = $(BUILD)/livepulser-agent$(EXE)
TEST_BIN = $(BUILD)/unit-tests$(EXE)
TESTING_BIN = $(BUILD)/livepulser-agent-testing$(EXE)

.PHONY: all clean test test-unit test-integration install uninstall

all: $(BIN)

$(BUILD):
	mkdir -p $@

$(BUILD)/cJSON.o: $(CJSON) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -c $< -o $@

$(BUILD)/cJSON-sanitize.o: $(CJSON) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZE) -w -c $< -o $@

$(BIN): $(SRC) $(BUILD)/cJSON.o include/*.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HARDEN) -Werror $(SRC) $(BUILD)/cJSON.o -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(CORE) $(PLATFORM) tests/unit.c $(BUILD)/cJSON-sanitize.o include/*.h | $(BUILD)
	$(CC) $(CPPFLAGS) -DLP_TESTING $(CFLAGS) $(SANITIZE) -Werror $(CORE) $(PLATFORM) tests/unit.c $(BUILD)/cJSON-sanitize.o -o $@ $(LDFLAGS) $(TEST_LIBS)

$(TESTING_BIN): $(SRC) $(BUILD)/cJSON-sanitize.o include/*.h | $(BUILD)
	$(CC) $(CPPFLAGS) -DLP_TESTING $(CFLAGS) $(SANITIZE) -Werror $(SRC) $(BUILD)/cJSON-sanitize.o -o $@ $(LDFLAGS) $(LDLIBS)

test: test-unit test-integration

test-unit: $(TEST_BIN)
	cd tests && ../$(TEST_BIN)

test-integration: $(TESTING_BIN)
	python3 tests/integration/run.py $(TESTING_BIN)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/livepulser-agent
ifeq ($(TARGET),darwin)
	install -d -m 0750 $(DESTDIR)$(PREFIX)/etc/livepulser-agent
	test -e $(DESTDIR)$(PREFIX)/etc/livepulser-agent/agent.conf || \
		install -m 0640 packaging/agent.conf.example $(DESTDIR)$(PREFIX)/etc/livepulser-agent/agent.conf
	install -d -m 0755 $(DESTDIR)/Library/LaunchDaemons
	install -m 0644 packaging/macos/com.livepulser.agent.plist $(DESTDIR)/Library/LaunchDaemons/com.livepulser.agent.plist
else
	install -d -m 0750 $(DESTDIR)/etc/livepulser-agent
	test -e $(DESTDIR)/etc/livepulser-agent/agent.conf || \
		install -m 0640 packaging/agent.conf.example $(DESTDIR)/etc/livepulser-agent/agent.conf
	install -d $(DESTDIR)/etc/systemd/system
	install -m 0644 packaging/systemd/livepulser-agent.service $(DESTDIR)/etc/systemd/system/livepulser-agent.service
endif

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/livepulser-agent
ifeq ($(TARGET),darwin)
	rm -f $(DESTDIR)/Library/LaunchDaemons/com.livepulser.agent.plist
else
	rm -f $(DESTDIR)/etc/systemd/system/livepulser-agent.service
endif

clean:
	rm -rf $(BUILD)
