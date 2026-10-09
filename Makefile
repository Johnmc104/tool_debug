# ══════════════════════════════════════════════════════════════════════════════
# tool_wave — FSDB Waveform & Netlist Signal Trace Tools
#   vwave   — 波形读取 (FSDB)
#   vsignal — 网表信号追踪 (KDB/RTL)
#
# 构建与发布 (由 common_packaging/packaging.mk 提供):
#   make build        — 构建二进制
#   make deploy-bin   — 部署到 $VTOOL_HOME/bin/
#   make package      — 打包为 tar.gz
#   make release      — 创建 git tag 并发布
#
# 编译:
#   make              — 编译 vwave + vsignal
#   make vwave        — 编译 CLI 与两个 worker
#   make vwave-ffr    — 仅编译无需 NPI 的 CLI 与 FFR worker
#   make vsignal      — 仅编译 vsignal
#
# 测试:
#   make test         — 运行全部测试
#   make clean        — 清理构建产物
# ══════════════════════════════════════════════════════════════════════════════

# ── 公共打包变量 (在 include 之前设置) ──────────────────────────────────────────
PROJECT_NAME  := tool_wave
BINARIES      := vwave vsignal vwave-npi-worker vwave-ffr-worker
PACKAGE_FILES := README.md .github/skills

# ── 引入公共打包目标: build / deploy-bin / package / release / tag / version ──
COMMON_PKG_DIR := ../common_packaging
-include $(COMMON_PKG_DIR)/packaging.mk

# ── Fallback (packaging.mk 不可用时) ──────────────────────────────────────────
VERSION       ?= $(shell cat VERSION 2>/dev/null || echo "0.0.0-dev")
RELEASE_DIR   ?= release
BIN_DIR       ?= $(RELEASE_DIR)/bin
DIST_DIR      ?= dist
_C_GREEN      ?= \033[0;32m
_C_YELLOW     ?= \033[1;33m
_C_BLUE       ?= \033[1;34m
_C_RESET      ?= \033[0m

# ── Verdi NPI paths ──────────────────────────────────────────────────────────
# 已验证: T-2022.06-SP2, Y-2026.03-SP2 (2026.03+ 需 License feature VerdiNPI)
VERDI_HOME   ?= /opt/Synopsys/verdi/Y-2026.03-SP2
FFR_SDK_HOME ?= /opt/Synopsys/verdi/T-2022.06-SP2
FFR_INC       = $(FFR_SDK_HOME)/share/FsdbReader
NPI_INC       = $(VERDI_HOME)/share/NPI/inc
NPI_L1_INC    = $(VERDI_HOME)/share/NPI/L1/C/inc
NPI_LIB_DIR   = $(VERDI_HOME)/share/NPI/lib/linux64

# ── Compiler ─────────────────────────────────────────────────────────────────
CXX          ?= g++
RUNTIME_LDFLAGS ?= -static-libstdc++ -static-libgcc
CXXFLAGS      = -std=c++14 -Wall -Wextra -O2 -DTW_BUILD_VERDI_HOME='"$(VERDI_HOME)"'
INCLUDES      = -I$(NPI_INC) -I$(NPI_L1_INC)
# RPATH (disable-new-dtags) 优先于 LD_LIBRARY_PATH，避免误加载其它 Verdi 版本的 libNPI
LDFLAGS       = -L$(NPI_LIB_DIR) -Wl,--disable-new-dtags,-rpath,$(NPI_LIB_DIR)
LIBS          = -lNPI -lnpiL1 -lpthread -lrt -ldl

# ── Shared common library ────────────────────────────────────────────────────
# Headers: #include "tw/json.h" → build/include/tw/ → src_common/
COMMON_DIR    = src_common
COMMON_HDRS   = $(wildcard $(COMMON_DIR)/*.h)
OBJ_DIR       = build
TW_INC_DIR    = $(OBJ_DIR)/include
TW_INC        = -I$(TW_INC_DIR)

# ── Binary targets ───────────────────────────────────────────────────────────
VWAVE_BIN     = $(BIN_DIR)/vwave
VSIGNAL_BIN   = $(BIN_DIR)/vsignal

# vwave sources
VWAVE_MAIN    = src_vwave/main.cpp
VWAVE_HDRS    = $(wildcard src_vwave/common/*.h src_vwave/server/*.h src_vwave/client/*.h src_vwave/backend/*.h)
VWAVE_INC     = -Isrc_vwave $(TW_INC) $(INCLUDES)

# vsignal sources
VSIGNAL_MAIN  = src_vsignal/main.cpp
VSIGNAL_HDRS  = $(wildcard src_vsignal/common/*.h src_vsignal/server/*.h src_vsignal/client/*.h)
VSIGNAL_INC   = -Isrc_vsignal $(TW_INC) $(INCLUDES)

# ── Default target ───────────────────────────────────────────────────────────
.DEFAULT_GOAL := all

.PHONY: all compile vwave vwave-ffr vsignal test test-vwave test-vsignal clean help

all: compile

compile: vwave $(VSIGNAL_BIN)

vwave: $(VWAVE_BIN) $(BIN_DIR)/vwave-npi-worker $(BIN_DIR)/vwave-ffr-worker
vwave-ffr: $(VWAVE_BIN) $(BIN_DIR)/vwave-ffr-worker
vsignal: $(VSIGNAL_BIN)

# Symlink so #include "tw/xxx.h" resolves to src_common/xxx.h
$(TW_INC_DIR)/tw: $(COMMON_HDRS)
	@mkdir -p $(TW_INC_DIR)
	@ln -sfn $(CURDIR)/$(COMMON_DIR) $@

$(VWAVE_BIN): $(VWAVE_MAIN) $(VWAVE_HDRS) $(COMMON_HDRS) $(TW_INC_DIR)/tw
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -Isrc_vwave $(TW_INC) -o $@ $(VWAVE_MAIN) $(RUNTIME_LDFLAGS) $(EXTRA_LDFLAGS) -lpthread -lrt -ldl
	@printf '%b\n' "$(_C_GREEN)[OK]$(_C_RESET)    $@ ($$(du -h $@ | cut -f1))"

$(VSIGNAL_BIN): $(VSIGNAL_MAIN) $(VSIGNAL_HDRS) $(COMMON_HDRS) $(TW_INC_DIR)/tw
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(VSIGNAL_INC) -o $@ $(VSIGNAL_MAIN) $(LDFLAGS) $(LIBS)
	@printf '%b\n' "$(_C_GREEN)[OK]$(_C_RESET)    $@ ($$(du -h $@ | cut -f1))"

# Isolated workers: only the NPI target links vendor libraries.
$(BIN_DIR)/vwave-npi-worker: src_vwave/worker.cpp $(VWAVE_HDRS) $(COMMON_HDRS) $(TW_INC_DIR)/tw
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(VWAVE_INC) -o $@ $< $(LDFLAGS) $(RUNTIME_LDFLAGS) $(EXTRA_LDFLAGS) $(LIBS)

$(BIN_DIR)/vwave-ffr-worker: src_vwave/worker.cpp $(VWAVE_HDRS) $(COMMON_HDRS) $(TW_INC_DIR)/tw
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -DWAVE_FFR_WORKER -Isrc_vwave $(TW_INC) -isystem $(FFR_INC) -o $@ $< $(RUNTIME_LDFLAGS) $(EXTRA_LDFLAGS) -lpthread -lrt -ldl

# ── Build hooks: C++ 原生编译覆盖 Docker/PyInstaller 流程 ────────────────────
_do-build _do-build-local:
	@printf '%b\n' "$(_C_BLUE)[BUILD]$(_C_RESET) $(PROJECT_NAME) v$(VERSION) — C++ native"
	@$(MAKE) --no-print-directory compile

# ── Test targets ──────────────────────────────────────────────────────────────
test: test-vwave test-vsignal

test-vwave: vwave
	@echo "\n══════ Running vwave tests ══════"
	BIN_DIR=$(abspath $(BIN_DIR)) bash test_vwave/run_test.sh

test-vsignal: $(VSIGNAL_BIN)
	@echo "\n══════ Running vsignal tests ══════"
	BIN_DIR=$(abspath $(BIN_DIR)) bash test_vsignal/run_test.sh

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(RELEASE_DIR) $(DIST_DIR) $(OBJ_DIR)
	@printf '%b\n' "$(_C_GREEN)[OK]$(_C_RESET)    清理完成"

# ── Help ──────────────────────────────────────────────────────────────────────
help: ## 显示帮助
	@echo "tool_wave v$(VERSION) — FSDB Waveform & Netlist Signal Trace Tools"
	@echo ""
	@echo "Compile:"
	@echo "  make               Compile vwave + vsignal"
	@echo "  make vwave         Compile vwave and both workers"
	@echo "  make vwave-ffr     Compile NPI-free CLI and FFR worker"
	@echo "  make vsignal       Compile vsignal only"
	@echo ""
	@echo "Build & Deploy:"
	@echo "  make build         Build binaries"
	@echo "  make deploy-bin    Install to \$$VTOOL_HOME/bin/"
	@echo "  make package       Create tar.gz archive with sha256"
	@echo "  make release       Create git tag and publish"
	@echo ""
	@echo "Test:"
	@echo "  make test          Run all tests"
	@echo "  make test-vwave    Run vwave tests"
	@echo "  make test-vsignal  Run vsignal tests"
	@echo ""
	@echo "Info:"
	@echo "  make version       Print version"
	@echo "  make pkg-info      Print packaging configuration"
	@echo "  make clean         Remove release/ dist/ build/"
	@echo ""
	@echo "Environment:"
	@echo "  VERDI_HOME=$(VERDI_HOME)"
	@echo "  FFR_SDK_HOME=$(FFR_SDK_HOME)"
	@echo "  CXX=$(CXX)"
