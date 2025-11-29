CC ?= gcc
ARM_CC ?= arm-linux-gnueabihf-gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSDDIR ?= /etc/systemd/system
SERVICE_SRC := scripts/forker.service
SERVICE_PATH := $(SYSDDIR)/forker.service
MONITOR_SCRIPT := scripts/monitor.sh
SHAPER_SCRIPT := scripts/shaper.sh
CONFIG_SRC := configs/forker.conf
TX_CONFIG_SRC := configs/tx-forker.conf
FORKER_CONF ?= /etc/forker.conf
FORKER_WORKDIR ?= /etc
BIN := forker
SRC := forker.c

.PHONY: all clean rebuild run install uninstall

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

rebuild: clean all

clean:
	rm -f $(BIN)

# Convenience: rebuild then run against the sample config.
run: $(BIN)
	./$(BIN) wfb.conf

# Cross-compilation helper for linux-arm targets.
arm: CC := $(ARM_CC)
arm: all

install: $(BIN) $(SERVICE_SRC) $(MONITOR_SCRIPT) $(SHAPER_SCRIPT) $(CONFIG_SRC) $(TX_CONFIG_SRC)
        install -d $(BINDIR) $(SYSDDIR) $(dir $(FORKER_CONF)) $(FORKER_WORKDIR)
        install -m 0755 $(BIN) $(BINDIR)/
        install -m 0755 $(MONITOR_SCRIPT) $(BINDIR)/monitor.sh
        install -m 0755 $(SHAPER_SCRIPT) $(BINDIR)/shaper.sh
        install -m 0644 $(CONFIG_SRC) $(FORKER_CONF)
        install -m 0644 $(TX_CONFIG_SRC) $(FORKER_WORKDIR)/tx-forker.conf
        sed -e 's|@FORKER_BIN@|$(BINDIR)/$(BIN)|g' \
            -e 's|@FORKER_CONF@|$(FORKER_CONF)|g' \
            -e 's|@VRX_DIR@|$(FORKER_WORKDIR)|g' \
            $(SERVICE_SRC) > $(SERVICE_PATH)
        systemctl daemon-reload

uninstall:
        rm -f $(BINDIR)/$(BIN) $(BINDIR)/monitor.sh $(BINDIR)/shaper.sh
        rm -f $(FORKER_CONF) $(FORKER_WORKDIR)/tx-forker.conf
        rm -f $(SERVICE_PATH)
        systemctl daemon-reload
