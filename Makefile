# ══════════════════════════════════════════════════════════════════════════════
# tool_wave — FSDB Waveform & Netlist Signal Trace Tools
#   vwave   — 波形读取 (FSDB)
#   vsignal — 网表信号追踪 (KDB/RTL)
#   vsignal-worker — NPI server for local/LSF execution nodes
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
BINARIES      := vwave vsignal vsignal-worker vsignal-worker-2022 vsignal-worker-2024 vsignal-worker-2026 vwave-npi-worker vwave-ffr-worker
PACKAGE_FILES := README.md

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
# 已验证: T-2022.06-SP2, W-2024.09, Y-2026.03-SP2
# VCS 版本由 test-vsignal 的 VCS_BIN/VCS_HOME 选择；它不参与 vsignal 链接。
VERDI_HOME   ?= /opt/Synopsys/verdi/Y-2026.03-SP2
FFR_SDK_HOME ?= /opt/Synopsys/verdi/T-2022.06-SP2
VERDI_2022_HOME ?= /opt/Synopsys/verdi/T-2022.06-SP2
VERDI_2024_HOME ?= /opt/Synopsys/verdi/W-2024.09
VERDI_2026_HOME ?= /opt/Synopsys/verdi/Y-2026.03-SP2
VCS_HOME     ?=
VCS_BIN      ?=
FFR_INC       = $(FFR_SDK_HOME)/share/FsdbReader
NPI_INC       = $(VERDI_HOME)/share/NPI/inc
NPI_L1_INC    = $(VERDI_HOME)/share/NPI/L1/C/inc
NPI_LIB_DIR   = $(VERDI_HOME)/share/NPI/lib/linux64

# ── Compiler ─────────────────────────────────────────────────────────────────
CXX          ?= g++
RUNTIME_LDFLAGS ?= -static-libstdc++ -static-libgcc
CXXFLAGS      = -std=c++14 -Wall -Wextra -O2 -DTW_BUILD_VERDI_HOME='"$(VERDI_HOME)"' -DTOOL_WAVE_VERSION='"$(VERSION)"'
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
VSIGNAL_WORKER_BIN = $(BIN_DIR)/vsignal-worker
VSIGNAL_WORKER_2022_BIN = $(BIN_DIR)/vsignal-worker-2022
VSIGNAL_WORKER_2024_BIN = $(BIN_DIR)/vsignal-worker-2024
VSIGNAL_WORKER_2026_BIN = $(BIN_DIR)/vsignal-worker-2026

# vwave sources
VWAVE_MAIN    = src_vwave/main.cpp
VWAVE_HDRS    = $(wildcard src_vwave/*.h src_vwave/common/*.h src_vwave/server/*.h src_vwave/client/*.h src_vwave/backend/*.h)
VWAVE_INC     = -Isrc_vwave $(TW_INC) $(INCLUDES)

# vsignal sources
VSIGNAL_MAIN  = src_vsignal/main.cpp
VSIGNAL_HDRS  = $(wildcard src_vsignal/common/*.h src_vsignal/server/*.h src_vsignal/client/*.h)
VSIGNAL_INC   = -Isrc_vsignal $(TW_INC)
VSIGNAL_WORKER_INC = -Isrc_vsignal $(TW_INC) $(INCLUDES)
VSIGNAL_WORKER_BASE_INC = -Isrc_vsignal $(TW_INC)

# ── Default target ───────────────────────────────────────────────────────────
.DEFAULT_GOAL := all

.PHONY: all compile vwave vwave-ffr vsignal test test-vwave test-vwave-lsf test-vsignal test-vsignal-lsf check-source-governance clean help

all: compile

compile: vwave vsignal

vwave: $(VWAVE_BIN) $(BIN_DIR)/vwave-npi-worker $(BIN_DIR)/vwave-ffr-worker
vwave-ffr: $(VWAVE_BIN) $(BIN_DIR)/vwave-ffr-worker
vsignal: $(VSIGNAL_BIN) $(VSIGNAL_WORKER_BIN) $(VSIGNAL_WORKER_2022_BIN) $(VSIGNAL_WORKER_2024_BIN) $(VSIGNAL_WORKER_2026_BIN)

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
	$(CXX) $(CXXFLAGS) $(VSIGNAL_INC) -o $@ $(VSIGNAL_MAIN) $(RUNTIME_LDFLAGS) $(EXTRA_LDFLAGS) -lpthread -lrt -ldl
	@printf '%b\n' "$(_C_GREEN)[OK]$(_C_RESET)    $@ ($$(du -h $@ | cut -f1))"

$(VSIGNAL_WORKER_BIN): src_vsignal/worker.cpp $(VSIGNAL_HDRS) $(COMMON_HDRS) $(TW_INC_DIR)/tw
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(VSIGNAL_WORKER_INC) -o $@ $< $(LDFLAGS) $(RUNTIME_LDFLAGS) $(EXTRA_LDFLAGS) $(LIBS)
	@printf '%b\n' "$(_C_GREEN)[OK]$(_C_RESET)    $@ ($$(du -h $@ | cut -f1))"

define build_vsignal_worker
	@mkdir -p $(BIN_DIR)
	$(CXX) $(filter-out -DTW_BUILD_VERDI_HOME=%,$(CXXFLAGS)) -DTW_BUILD_VERDI_HOME='"$(1)"' $(VSIGNAL_WORKER_BASE_INC) -I$(1)/share/NPI/inc -I$(1)/share/NPI/L1/C/inc -o $@ $< -L$(1)/share/NPI/lib/linux64 -Wl,--disable-new-dtags,-rpath,$(1)/share/NPI/lib/linux64 $(RUNTIME_LDFLAGS) $(EXTRA_LDFLAGS) -lNPI -lnpiL1 -lpthread -lrt -ldl
	@printf '%b\n' "$(_C_GREEN)[OK]$(_C_RESET)    $@ ($$(du -h $@ | cut -f1))"
endef

$(VSIGNAL_WORKER_2022_BIN): src_vsignal/worker.cpp $(VSIGNAL_HDRS) $(COMMON_HDRS) $(TW_INC_DIR)/tw
	$(call build_vsignal_worker,$(VERDI_2022_HOME))

$(VSIGNAL_WORKER_2024_BIN): src_vsignal/worker.cpp $(VSIGNAL_HDRS) $(COMMON_HDRS) $(TW_INC_DIR)/tw
	$(call build_vsignal_worker,$(VERDI_2024_HOME))

$(VSIGNAL_WORKER_2026_BIN): src_vsignal/worker.cpp $(VSIGNAL_HDRS) $(COMMON_HDRS) $(TW_INC_DIR)/tw
	$(call build_vsignal_worker,$(VERDI_2026_HOME))

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
test: check-source-governance test-vwave test-vsignal

check-source-governance:
	@set -eu; \
	bad="$$(rg -n '#include.*(src_vwave|src_vsignal|NPI|FsdbReader)' src_common || true)"; \
	if [ -n "$$bad" ]; then printf '%s\n' "[ERR] src_common has tool/vendor dependency:"; printf '%s\n' "$$bad"; exit 1; fi; \
	duplicate="$$(rg -n '^inline[[:space:]].*(hex_encode|hex_decode|split_words|replace_pid|add_status_fields)' src_vwave src_vsignal || true)"; \
	if [ -n "$$duplicate" ]; then printf '%s\n' "[ERR] duplicated shared transport helper:"; printf '%s\n' "$$duplicate"; exit 1; fi; \
	for bin in vwave vwave-npi-worker vwave-ffr-worker vsignal vsignal-worker vsignal-worker-2022 vsignal-worker-2024 vsignal-worker-2026; do \
		printf '%s\n' "$(BINARIES)" | tr ' ' '\n' | grep -qx "$$bin" || { printf '%s\n' "[ERR] package manifest missing $$bin"; exit 1; }; \
	done; \
	printf '%b\n' "$(_C_GREEN)[OK]$(_C_RESET)    source governance checks passed"

test-vwave: vwave
	@echo "\n══════ Running vwave tests ══════"
	BIN_DIR=$(abspath $(BIN_DIR)) bash test_vwave/run_test.sh

test-vwave-lsf: vwave
	@echo "\n══════ Running vwave LSF proxy test ══════"
	python3 test_vwave/test_lsf_mode.py

test-vsignal: vsignal
	@echo "\n══════ Running vsignal tests ══════"
	BIN_DIR=$(abspath $(BIN_DIR)) VCS_HOME="$(VCS_HOME)" VCS_BIN="$(VCS_BIN)" bash test_vsignal/run_test.sh

test-vsignal-lsf: vsignal
	@echo "\n══════ Running vsignal LSF proxy test ══════"
	python3 test_vsignal/test_lsf_mode.py

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
	@echo "  make test-vwave-lsf Run LSF proxy smoke test"
	@echo "  make test-vsignal  Run vsignal tests"
	@echo "  make test-vsignal-lsf Run vsignal LSF proxy smoke test"
	@echo "  make check-source-governance Check shared-layer boundaries and package manifest"
	@echo ""
	@echo "Info:"
	@echo "  make version       Print version"
	@echo "  make pkg-info      Print packaging configuration"
	@echo "  make clean         Remove release/ dist/ build/"
	@echo ""
	@echo "Environment:"
	@echo "  VERDI_HOME=$(VERDI_HOME)"
	@echo "  VERDI_2022_HOME=$(VERDI_2022_HOME)"
	@echo "  VERDI_2024_HOME=$(VERDI_2024_HOME)"
	@echo "  VERDI_2026_HOME=$(VERDI_2026_HOME)"
	@echo "  FFR_SDK_HOME=$(FFR_SDK_HOME)"
	@echo "  VCS_HOME=$(if $(VCS_HOME),$(VCS_HOME),(由 PATH 查找))"
	@echo "  VCS_BIN=$(if $(VCS_BIN),$(VCS_BIN),(由 VCS_HOME/PATH 查找))"
	@echo "  VWAVE_RUN_MODE=local|lsf (LSF 模式可配 VWAVE_LSF_BSUB/BKILL/QUEUE/RESOURCE)"
	@echo "  vsignal 复用 VWAVE_RUN_MODE 与 VWAVE_LSF_* 环境变量"
	@echo "  CXX=$(CXX)"
