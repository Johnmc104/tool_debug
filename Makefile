# ══════════════════════════════════════════════════════════════════════════════
# vwave — FSDB Waveform Reader — Build System
# ══════════════════════════════════════════════════════════════════════════════

# ─── Verdi NPI paths ──────────────────────────────────────────────────────────
VERDI_HOME   ?= /opt/Synopsys/verdi/T-2022.06-SP2
NPI_INC       = $(VERDI_HOME)/share/NPI/inc
NPI_L1_INC    = $(VERDI_HOME)/share/NPI/L1/C/inc
NPI_LIB_DIR   = $(VERDI_HOME)/share/NPI/lib/linux64

# ─── Compiler ─────────────────────────────────────────────────────────────────
CXX          ?= g++
CXXFLAGS      = -std=c++14 -Wall -Wextra -O2
INCLUDES      = -Isrc -I$(NPI_INC) -I$(NPI_L1_INC)
LDFLAGS       = -L$(NPI_LIB_DIR) -Wl,-rpath,$(NPI_LIB_DIR)
LIBS          = -lNPI -lnpiL1 -lpthread -lrt -ldl

# ─── Output ───────────────────────────────────────────────────────────────────
BUILD_DIR     = build
BIN_DIR       = $(BUILD_DIR)/bin
VWAVE_BIN     = $(BIN_DIR)/vwave

MAIN_SRC      = src/main.cpp
HEADERS       = src/common/protocol.h src/common/json_parser.h src/common/run_dir.h \
                src/server/server_core.h src/client/client_core.h

# ─── Targets ──────────────────────────────────────────────────────────────────
.PHONY: all clean help

all: $(VWAVE_BIN)

$(VWAVE_BIN): $(MAIN_SRC) $(HEADERS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(MAIN_SRC) $(LDFLAGS) $(LIBS)
	@echo "Built: $@"

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned."

help:
	@echo "vwave — FSDB Waveform Reader"
	@echo ""
	@echo "  make           Build vwave (default)"
	@echo "  make clean     Remove build artifacts"
	@echo ""
	@echo "Environment:"
	@echo "  VERDI_HOME=$(VERDI_HOME)"
	@echo "  CXX=$(CXX)"
	@echo ""
	@echo "Quick start:"
	@echo "  $(VWAVE_BIN) open test/tb_top.fsdb"
	@echo "  $(VWAVE_BIN) info"
	@echo "  $(VWAVE_BIN) scopes"
	@echo "  $(VWAVE_BIN) get-value -s tb.intf.clk -t 1000"
	@echo "  $(VWAVE_BIN) close"
