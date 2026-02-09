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
VSIGNAL_BIN   = $(BIN_DIR)/vsignal

MAIN_SRC      = src/main.cpp
HEADERS       = src/common/protocol.h src/common/json_parser.h src/common/run_dir.h \
                src/server/server_core.h src/client/client_core.h

VSIGNAL_MAIN  = src_vsignal/main.cpp
VSIGNAL_HDRS  = src_vsignal/common/protocol.h src_vsignal/common/json_parser.h \
                src_vsignal/common/run_dir.h src_vsignal/server/server_core.h \
                src_vsignal/client/client_core.h
VSIGNAL_INC   = -Isrc_vsignal -I$(NPI_INC) -I$(NPI_L1_INC)

# ─── Targets ──────────────────────────────────────────────────────────────────
.PHONY: all clean help vwave vsignal

all: $(VWAVE_BIN) $(VSIGNAL_BIN)

vwave: $(VWAVE_BIN)
vsignal: $(VSIGNAL_BIN)

$(VWAVE_BIN): $(MAIN_SRC) $(HEADERS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(MAIN_SRC) $(LDFLAGS) $(LIBS)
	@echo "Built: $@"

$(VSIGNAL_BIN): $(VSIGNAL_MAIN) $(VSIGNAL_HDRS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(VSIGNAL_INC) -o $@ $(VSIGNAL_MAIN) $(LDFLAGS) $(LIBS)
	@echo "Built: $@"

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned."

help:
	@echo "vwave + vsignal — Build System"
	@echo ""
	@echo "  make           Build both vwave and vsignal"
	@echo "  make vwave     Build vwave only"
	@echo "  make vsignal   Build vsignal only"
	@echo "  make clean     Remove build artifacts"
	@echo ""
	@echo "Environment:"
	@echo "  VERDI_HOME=$(VERDI_HOME)"
	@echo "  CXX=$(CXX)"
	@echo ""
	@echo "vwave quick start:"
	@echo "  $(VWAVE_BIN) open test/tb_top.fsdb"
	@echo "  $(VWAVE_BIN) get-value -s tb.intf.clk -t 1000"
	@echo "  $(VWAVE_BIN) close"
	@echo ""
	@echo "vsignal quick start:"
	@echo "  $(VSIGNAL_BIN) open -dbdir simv.daidir"
	@echo "  $(VSIGNAL_BIN) driver top.sig_a"
	@echo "  $(VSIGNAL_BIN) load top.sig_b"
	@echo "  $(VSIGNAL_BIN) fanin top.q_reg"
	@echo "  $(VSIGNAL_BIN) close"
