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

# ─── Shared common library ────────────────────────────────────────────────────
# Headers included as #include "tw/json.h" etc.  We create a build-time
# symlink  build/include/tw → src_common  so -Ibuild/include resolves them.
COMMON_DIR    = src_common
COMMON_HDRS   = $(wildcard $(COMMON_DIR)/*.h)
TW_INC_DIR    = $(BUILD_DIR)/include
TW_INC        = -I$(TW_INC_DIR)

# ─── Output ───────────────────────────────────────────────────────────────────
BUILD_DIR     = build
BIN_DIR       = $(BUILD_DIR)/bin
VWAVE_BIN     = $(BIN_DIR)/vwave
VSIGNAL_BIN   = $(BIN_DIR)/vsignal

# ─── Distribution ─────────────────────────────────────────────────────────────
VERSION      ?= $(shell git describe --tags --always 2>/dev/null || echo dev)
DIST_NAME     = tool_wave-$(VERSION)
DIST_DIR      = $(BUILD_DIR)/$(DIST_NAME)
SKILL_DIR     = .github/skills/tool-wave

# vwave sources
VWAVE_MAIN    = src_vwave/main.cpp
VWAVE_HDRS    = $(wildcard src_vwave/common/*.h src_vwave/server/*.h src_vwave/client/*.h)
VWAVE_INC     = -Isrc_vwave $(TW_INC) $(INCLUDES)

# vsignal sources
VSIGNAL_MAIN  = src_vsignal/main.cpp
VSIGNAL_HDRS  = $(wildcard src_vsignal/common/*.h src_vsignal/server/*.h src_vsignal/client/*.h)
VSIGNAL_INC   = -Isrc_vsignal $(TW_INC) $(INCLUDES)

# ─── Targets ──────────────────────────────────────────────────────────────────
.PHONY: all clean help vwave vsignal test test-vwave test-vsignal dist

all: $(VWAVE_BIN) $(VSIGNAL_BIN)

vwave: $(VWAVE_BIN)
vsignal: $(VSIGNAL_BIN)

# Symlink so #include "tw/xxx.h" resolves to src_common/xxx.h
$(TW_INC_DIR)/tw: $(COMMON_HDRS)
	@mkdir -p $(TW_INC_DIR)
	@ln -sfn $(CURDIR)/$(COMMON_DIR) $@

$(VWAVE_BIN): $(VWAVE_MAIN) $(VWAVE_HDRS) $(COMMON_HDRS) $(TW_INC_DIR)/tw
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(VWAVE_INC) -o $@ $(VWAVE_MAIN) $(LDFLAGS) $(LIBS)
	@echo "Built: $@"

$(VSIGNAL_BIN): $(VSIGNAL_MAIN) $(VSIGNAL_HDRS) $(COMMON_HDRS) $(TW_INC_DIR)/tw
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

# ─── Distribution target ──────────────────────────────────────────────────────
dist: $(VWAVE_BIN) $(VSIGNAL_BIN)
	@rm -rf $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/bin
	@mkdir -p $(DIST_DIR)/skills/tool-wave/references
	@mkdir -p $(DIST_DIR)/skills/tool-wave/scripts
	cp $(VWAVE_BIN) $(VSIGNAL_BIN) $(DIST_DIR)/bin/
	cp $(SKILL_DIR)/SKILL.md               $(DIST_DIR)/skills/tool-wave/
	cp $(SKILL_DIR)/references/vwave.md    $(DIST_DIR)/skills/tool-wave/references/
	cp $(SKILL_DIR)/references/vsignal.md  $(DIST_DIR)/skills/tool-wave/references/
	cp $(SKILL_DIR)/scripts/check-tools.sh $(DIST_DIR)/skills/tool-wave/scripts/
	cp README.md $(DIST_DIR)/
	@cd $(BUILD_DIR) && tar czf $(DIST_NAME).tar.gz $(DIST_NAME)/
	@echo "Package: $(BUILD_DIR)/$(DIST_NAME).tar.gz"

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
	@echo "  make dist          Package binaries + skill + README"
	@echo ""
	@echo "Test:"
	@echo "  make test          Run all tests (vwave + vsignal)"
	@echo "  make test-vwave    Run vwave tests (62 cases)"
	@echo "  make test-vsignal  Run vsignal tests (27 cases)"
	@echo ""
	@echo "Environment:"
	@echo "  VERDI_HOME=$(VERDI_HOME)"
	@echo "  CXX=$(CXX)"
