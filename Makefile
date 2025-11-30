CC ?= gcc
ARM_CC ?= arm-linux-gnueabihf-gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSDDIR ?= /etc/systemd/system
SERVICE_SRC := scripts/wfb_supervisor.service
SERVICE_PATH := $(SYSDDIR)/wfb_supervisor.service
MONITOR_SCRIPT := scripts/monitor.sh
SHAPER_SCRIPT := scripts/shaper.sh
CONFIG_SRC := wfb.conf
TX_CONFIG_SRC := configs/tx-wfb.conf
SUPERVISOR_CONF ?= /etc/wfb_supervisor.conf
SUPERVISOR_WORKDIR ?= /etc
BIN := wfb_supervisor
SRC := wfb_supervisor.c

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
	install -d $(BINDIR) $(SYSDDIR) $(dir $(SUPERVISOR_CONF)) $(SUPERVISOR_WORKDIR)
	install -m 0755 $(BIN) $(BINDIR)/
	install -m 0755 $(MONITOR_SCRIPT) $(BINDIR)/monitor.sh
	install -m 0755 $(SHAPER_SCRIPT) $(BINDIR)/shaper.sh
	install -m 0644 $(CONFIG_SRC) $(SUPERVISOR_CONF)
	install -m 0644 $(TX_CONFIG_SRC) $(SUPERVISOR_WORKDIR)/tx-wfb.conf
	sed -e 's|@WFB_SUPERVISOR_BIN@|$(BINDIR)/$(BIN)|g' \
    -e 's|@WFB_SUPERVISOR_CONF@|$(SUPERVISOR_CONF)|g' \
    -e 's|@VRX_DIR@|$(SUPERVISOR_WORKDIR)|g' \
$(SERVICE_SRC) > $(SERVICE_PATH)
	systemctl daemon-reload

uninstall:
	rm -f $(BINDIR)/$(BIN) $(BINDIR)/monitor.sh $(BINDIR)/shaper.sh
	rm -f $(SUPERVISOR_CONF) $(SUPERVISOR_WORKDIR)/tx-wfb.conf
	rm -f $(SERVICE_PATH)
	systemctl daemon-reload
