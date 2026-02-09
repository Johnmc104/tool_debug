# ══════════════════════════════════════════════════════════════════════════════
# tool_wave — FSDB Waveform & Netlist Signal Trace Tools
#   vwave   — 波形读取 (FSDB)
#   vsignal — 网表信号追踪 (KDB/RTL)
# ══════════════════════════════════════════════════════════════════════════════

# ─── Verdi NPI paths ──────────────────────────────────────────────────────────
VERDI_HOME   ?= /opt/Synopsys/verdi/T-2022.06-SP2
NPI_INC       = $(VERDI_HOME)/share/NPI/inc
NPI_L1_INC    = $(VERDI_HOME)/share/NPI/L1/C/inc
NPI_LIB_DIR   = $(VERDI_HOME)/share/NPI/lib/linux64

# ─── Compiler ─────────────────────────────────────────────────────────────────
CXX          ?= g++
CXXFLAGS      = -std=c++14 -Wall -Wextra -O2
INCLUDES      = -I$(NPI_INC) -I$(NPI_L1_INC)
LDFLAGS       = -L$(NPI_LIB_DIR) -Wl,-rpath,$(NPI_LIB_DIR)
LIBS          = -lNPI -lnpiL1 -lpthread -lrt -ldl

# ─── Output ───────────────────────────────────────────────────────────────────
BUILD_DIR     = build
BIN_DIR       = $(BUILD_DIR)/bin
VWAVE_BIN     = $(BIN_DIR)/vwave
VSIGNAL_BIN   = $(BIN_DIR)/vsignal

# vwave sources
VWAVE_MAIN    = src_vwave/main.cpp
VWAVE_HDRS    = $(wildcard src_vwave/common/*.h src_vwave/server/*.h src_vwave/client/*.h)
VWAVE_INC     = -Isrc_vwave $(INCLUDES)

# vsignal sources
VSIGNAL_MAIN  = src_vsignal/main.cpp
VSIGNAL_HDRS  = $(wildcard src_vsignal/common/*.h src_vsignal/server/*.h src_vsignal/client/*.h)
VSIGNAL_INC   = -Isrc_vsignal $(INCLUDES)

# ─── Targets ──────────────────────────────────────────────────────────────────
.PHONY: all clean help vwave vsignal test test-vwave test-vsignal

all: $(VWAVE_BIN) $(VSIGNAL_BIN)

vwave: $(VWAVE_BIN)
vsignal: $(VSIGNAL_BIN)

$(VWAVE_BIN): $(VWAVE_MAIN) $(VWAVE_HDRS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(VWAVE_INC) -o $@ $(VWAVE_MAIN) $(LDFLAGS) $(LIBS)
	@echo "Built: $@"

$(VSIGNAL_BIN): $(VSIGNAL_MAIN) $(VSIGNAL_HDRS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(VSIGNAL_INC) -o $@ $(VSIGNAL_MAIN) $(LDFLAGS) $(LIBS)
	@echo "Built: $@"

# ─── Test targets ─────────────────────────────────────────────────────────────
test: test-vwave test-vsignal

test-vwave: $(VWAVE_BIN)
	@echo "\n══════ Running vwave tests ══════"
	bash test_vwave/run_test.sh

test-vsignal: $(VSIGNAL_BIN)
	@echo "\n══════ Running vsignal tests ══════"
	bash test_vsignal/run_test.sh

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned."

help:
	@echo "tool_wave — FSDB Waveform & Netlist Signal Trace Tools"
	@echo ""
	@echo "Build:"
	@echo "  make               Build both vwave and vsignal"
	@echo "  make vwave         Build vwave only"
	@echo "  make vsignal       Build vsignal only"
	@echo "  make clean         Remove build artifacts"
	@echo ""
	@echo "Test:"
	@echo "  make test          Run all tests (vwave + vsignal)"
	@echo "  make test-vwave    Run vwave tests (62 cases)"
	@echo "  make test-vsignal  Run vsignal tests (27 cases)"
	@echo ""
	@echo "Environment:"
	@echo "  VERDI_HOME=$(VERDI_HOME)"
	@echo "  CXX=$(CXX)"
