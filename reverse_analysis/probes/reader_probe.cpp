// FsdbReader 对照探针：不使用 NPI；仅验证指定样本的层次和数字信号事件。
#include "ffrAPI.h"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

struct Variable {
    std::string path;
    fsdbVarIdcode id;
    int left, right;
};
struct Tree {
    std::vector<std::string> scopes;
    std::vector<Variable> variables;
    unsigned scope_count = 0;
};

static std::string quoted(const std::string& text) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += c;
    }
    return out + '"';
}
static uint64_t timestamp(const fsdbTag64& tag) {
    return (uint64_t(tag.H) << 32) | tag.L;
}
static bool_T tree_callback(fsdbTreeCBType type, void* client, void* data) {
    auto& tree = *static_cast<Tree*>(client);
    if (type == FSDB_TREE_CBT_SCOPE) {
        auto* scope = static_cast<fsdbTreeCBDataScope*>(data);
        tree.scopes.emplace_back(scope->name ? scope->name : "");
        ++tree.scope_count;
    } else if (type == FSDB_TREE_CBT_UPSCOPE) {
        if (!tree.scopes.empty()) tree.scopes.pop_back();
    } else if (type == FSDB_TREE_CBT_VAR) {
        auto* var = static_cast<fsdbTreeCBDataVar*>(data);
        std::string path;
        for (const auto& scope : tree.scopes) {
            if (!path.empty()) path += '.';
            path += scope;
        }
        if (!path.empty()) path += '.';
        path += var->name ? var->name : "";
        tree.variables.push_back({path, var->u.idcode, var->lbitnum, var->rbitnum});
    }
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: reader_probe file.fsdb full.signal.name\n";
        return 2;
    }
    ffrObject* reader = ffrObject::ffrOpen3(argv[1]);
    if (!reader) { std::cerr << "ffrOpen3 failed\n"; return 1; }
    Tree tree;
    reader->ffrSetTreeCBFunc(tree_callback, &tree);
    if (reader->ffrReadScopeVarTree() != FSDB_RC_SUCCESS) {
        std::cerr << "tree read failed\n"; reader->ffrClose(); return 1;
    }
    fsdbTag64 first{}, last{};
    if (reader->ffrGetMinFsdbTag64(&first) != FSDB_RC_SUCCESS ||
        reader->ffrGetMaxFsdbTag64(&last) != FSDB_RC_SUCCESS) {
        std::cerr << "time range read failed\n"; reader->ffrClose(); return 1;
    }
    std::cout << "{\"type\":\"file\",\"scale\":" << quoted(reader->ffrGetScaleUnit())
              << ",\"min_time\":" << timestamp(first) << ",\"max_time\":" << timestamp(last)
              << ",\"scope_count\":" << tree.scope_count
              << ",\"variable_records\":" << tree.variables.size() << "}\n";
    const Variable* chosen = nullptr;
    for (const auto& var : tree.variables) {
        if (var.path == argv[2]) { chosen = &var; break; }
    }
    if (!chosen) {
        std::cerr << "signal not found; first 20 variable paths:\n";
        for (size_t i = 0; i < tree.variables.size() && i < 20; ++i)
            std::cerr << tree.variables[i].path << '\n';
        reader->ffrClose(); return 1;
    }
    if (reader->ffrAddToSignalList(chosen->id) != FSDB_RC_SUCCESS ||
        reader->ffrLoadSignals() != FSDB_RC_SUCCESS) {
        std::cerr << "signal load failed\n"; reader->ffrClose(); return 1;
    }
    ffrVCTrvsHdl handle = reader->ffrCreateVCTraverseHandle(chosen->id);
    if (!handle) { std::cerr << "traverse creation failed\n"; reader->ffrClose(); return 1; }
    if (handle->ffrGetBytesPerBit() != FSDB_BYTES_PER_BIT_1B || !handle->ffrHasIncoreVC()) {
        std::cerr << "probe supports populated 1-byte-per-bit digital signals only\n";
        handle->ffrFree(); reader->ffrClose(); return 1;
    }
    std::cout << "{\"type\":\"signal\",\"path\":" << quoted(chosen->path)
              << ",\"id\":" << chosen->id << ",\"left\":" << chosen->left
              << ",\"right\":" << chosen->right << ",\"bits\":" << handle->ffrGetBitSize() << "}\n";
    unsigned count = 0;
    fsdbRC rc = handle->ffrGotoTheFirstVC();
    while (rc == FSDB_RC_SUCCESS && count < 10000) {
        fsdbTag64 tag{};
        byte_T* raw = nullptr;
        if (handle->ffrGetXTag(&tag) != FSDB_RC_SUCCESS ||
            handle->ffrGetVC(&raw) != FSDB_RC_SUCCESS || !raw) {
            std::cerr << "event read failed\n";
            handle->ffrFree(); reader->ffrClose(); return 1;
        }
        std::string value;
        for (unsigned bit = 0; bit < handle->ffrGetBitSize(); ++bit)
            value += raw[bit] < 4 ? "01xz"[raw[bit]] : '?';
        std::cout << "{\"type\":\"event\",\"time\":" << timestamp(tag)
                  << ",\"value\":" << quoted(value) << "}\n";
        ++count;
        rc = handle->ffrGotoNextVC();
    }
    std::cout << "{\"type\":\"summary\",\"events\":" << count
              << ",\"limit_reached\":" << (rc == FSDB_RC_SUCCESS ? "true" : "false") << "}\n";
    handle->ffrFree();
    reader->ffrClose();
    return 0;
}
