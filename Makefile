CC ?= gcc
ARM_CC ?= arm-linux-gnueabihf-gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSDDIR ?= /etc/systemd/system
SERVICE := forker.service
SERVICE_PATH := $(SYSDDIR)/forker.service
FORKER_CONF ?= /etc/forker.conf
FORKER_WORKDIR ?= /etc
BIN := forker
SRC := forker.c

.PHONY: all clean rebuild run install

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

install: $(BIN) $(SERVICE)
	install -d $(BINDIR) $(SYSDDIR)
	install -m 0755 $(BIN) $(BINDIR)/
	sed -e 's|@FORKER_BIN@|$(BINDIR)/$(BIN)|g' \
	    -e 's|@FORKER_CONF@|$(FORKER_CONF)|g' \
	    -e 's|@VRX_DIR@|$(FORKER_WORKDIR)|g' \
	    $(SERVICE) > $(SERVICE_PATH)
	systemctl daemon-reload
