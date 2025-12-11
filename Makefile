CC ?= gcc
ARM_CC ?= arm-linux-gnueabihf-gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSDDIR ?= /etc/systemd/system
DESTDIR ?=
INSTALL ?= install
SERVICE_SRC := scripts/wfb_supervisor.service
SERVICE_PATH := $(SYSDDIR)/wfb_supervisor.service
MONITOR_SCRIPT := scripts/monitor.sh
SHAPER_SCRIPT := scripts/shaper.sh
CONFIG_SRC := config/wfb.conf
SUPERVISOR_CONF ?= /etc/wfb.conf
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
	./$(BIN) $(CONFIG_SRC)

# Cross-compilation helper for linux-arm targets.
arm: CC := $(ARM_CC)
arm: all

install: $(BIN) $(SERVICE_SRC) $(MONITOR_SCRIPT) $(SHAPER_SCRIPT) $(CONFIG_SRC)
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(SYSDDIR) $(DESTDIR)$(dir $(SUPERVISOR_CONF)) $(DESTDIR)$(SUPERVISOR_WORKDIR)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/
	$(INSTALL) -m 0755 $(MONITOR_SCRIPT) $(DESTDIR)$(BINDIR)/monitor.sh
	$(INSTALL) -m 0755 $(SHAPER_SCRIPT) $(DESTDIR)$(BINDIR)/shaper.sh
	$(INSTALL) -m 0644 $(CONFIG_SRC) $(DESTDIR)$(SUPERVISOR_CONF)
	sed -e 's|@WFB_SUPERVISOR_BIN@|$(BINDIR)/$(BIN)|g' \
	    -e 's|@WFB_SUPERVISOR_CONF@|$(SUPERVISOR_CONF)|g' \
	    -e 's|@VRX_DIR@|$(SUPERVISOR_WORKDIR)|g' \
	$(SERVICE_SRC) > $(DESTDIR)$(SERVICE_PATH)
	if [ -z "$(DESTDIR)" ]; then systemctl daemon-reload; fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN) $(DESTDIR)$(BINDIR)/monitor.sh $(DESTDIR)$(BINDIR)/shaper.sh
	rm -f $(DESTDIR)$(SUPERVISOR_CONF)
	rm -f $(DESTDIR)$(SERVICE_PATH)
	if [ -z "$(DESTDIR)" ]; then systemctl daemon-reload; fi
