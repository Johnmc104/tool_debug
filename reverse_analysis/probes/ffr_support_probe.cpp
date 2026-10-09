// 独立 SDK 对照探针：记录树回调及 real/string 原始 VC，不链接 NPI。
#include "ffrAPI.h"
#include "tw/json.h"
#include <cstring>
#include <iostream>
#include <vector>

struct Variable { std::string name; fsdbVarIdcode id; unsigned type; };
static bool reading_types = false;
static bool_T tree(fsdbTreeCBType type, void* client, void* data) {
    auto& variables = *static_cast<std::vector<Variable>*>(client);
    tw::JsonObject j; j.set("callback", int64_t(type));
    j.set("phase", std::string(reading_types ? "datatype" : "tree"));
    if (type == FSDB_TREE_CBT_SCOPE) {
        j.set("name", static_cast<fsdbTreeCBDataScope*>(data)->name);
    } else if (type == FSDB_TREE_CBT_VAR) {
        auto* v = static_cast<fsdbTreeCBDataVar*>(data);
        j.set("name", v->name); j.set("id", int64_t(v->u.idcode));
        j.set("type", int64_t(v->type)); j.set("bytes_per_bit", int64_t(v->bytes_per_bit));
        j.set("left", int64_t(v->lbitnum)); j.set("right", int64_t(v->rbitnum));
        j.set("dtidcode", int64_t(v->dtidcode));
        variables.push_back({v->name, v->u.idcode, unsigned(v->type & 0x3f)});
    } else if (type == FSDB_TREE_CBT_STRUCT_BEGIN) {
        auto* s = static_cast<fsdbTreeCBDataStructBegin*>(data);
        j.set("name", s->name); j.set("type", int64_t(s->type));
    } else if (type == FSDB_TREE_CBT_ARRAY_BEGIN) {
        auto* a = static_cast<fsdbTreeCBDataArrayBegin*>(data);
        j.set("name", a->name); j.set_bool("packed", a->is_packed_array);
    }
    std::cout << j.dump() << '\n';
    return 1;
}
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    auto* file = ffrObject::ffrOpen3(argv[1]);
    if (!file) return 2;
    std::vector<Variable> variables;
    file->ffrSetTreeCBFunc(tree, &variables);
    if (file->ffrHasDataTypeDef()) {
        uint_T block = 0;
        reading_types = true;
        if (file->ffrReadDataTypeDefByBlkIdx(block) != FSDB_RC_SUCCESS) {
            file->ffrClose(); return 6;
        }
        reading_types = false;
    }
    if (file->ffrReadScopeVarTree() != FSDB_RC_SUCCESS) { file->ffrClose(); return 3; }
    for (const auto& v : variables) {
        if (v.type != FSDB_VT_VCD_REAL && v.type != FSDB_VT_STRING) continue;
        if (file->ffrAddToSignalList(v.id) != FSDB_RC_SUCCESS ||
            file->ffrLoadSignals() != FSDB_RC_SUCCESS) { file->ffrClose(); return 4; }
        auto* cursor = file->ffrCreateVCTraverseHandle(v.id);
        if (!cursor) { file->ffrClose(); return 5; }
        if (cursor->ffrGotoTheFirstVC() == FSDB_RC_SUCCESS) do {
            fsdbTag64 t{}; byte_T* raw = nullptr;
            if (cursor->ffrGetXTag(&t) != FSDB_RC_SUCCESS ||
                cursor->ffrGetVC(&raw) != FSDB_RC_SUCCESS || !raw) continue;
            tw::JsonObject j; j.set("name", v.name); j.set("event", std::string("raw_value"));
            j.set("time", int64_t((uint64_t(t.H) << 32) | t.L));
            j.set("bytes_per_bit", int64_t(cursor->ffrGetBytesPerBit()));
            j.set("byte_count", int64_t(cursor->ffrGetByteCount()));
            if (v.type == FSDB_VT_VCD_REAL && cursor->ffrGetBytesPerBit() == FSDB_BYTES_PER_BIT_8B) {
                double value; std::memcpy(&value, raw, sizeof(value));
                j.set_raw("real", std::to_string(value));
            }
            // string 本轮只核实事件和元数据，不在未确认布局前解引用为字符指针。
            std::cout << j.dump() << '\n';
        } while (cursor->ffrGotoNextVC() == FSDB_RC_SUCCESS);
        cursor->ffrFree();
    }
    file->ffrClose(); return 0;
}
