#ifndef WAVE_FFR_BACKEND_H
#define WAVE_FFR_BACKEND_H
#include "backend/backend.h"
#include "common/session.h"
#include "ffrAPI.h"
#include <dlfcn.h>
#include <link.h>
#include <cstring>
#include <memory>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace wave {

// Linux x86_64 / Itanium C++ ABI 适配。虚方法由 SDK 接口调用；普通成员入口
// 显式绑定为带 this 参数的函数。这里只接受已验证的 reader 构建。
class FfrBackend : public WaveBackend {
    void *zlib_=nullptr, *support_=nullptr, *library_=nullptr;
    ffrObject* file_=nullptr;
    struct Ops {
        fsdbRC (*first)(ffrVCIterOne*) = nullptr;
        fsdbRC (*next)(ffrVCIterOne*) = nullptr;
        fsdbRC (*prev)(ffrVCIterOne*) = nullptr;
        fsdbRC (*seek)(ffrVCIterOne*,void*,int*) = nullptr;
        fsdbRC (*tag)(ffrVCIterOne*,void*) = nullptr;
        fsdbRC (*value)(ffrVCIterOne*,byte_T**) = nullptr;
        uint_T (*bits)(ffrVCIterOne*) = nullptr;
        fsdbBytesPerBit (*bytes)(ffrVCIterOne*) = nullptr;
        void (*free)(ffrVCIterOne*) = nullptr;
    } ops_;
    ffrObject* (*open_)(str_T)=nullptr;
    fsdbRC (*file_info_)(str_T,ffrFSDBInfo&)=nullptr;
    struct Var { SignalInfo info; fsdbVarIdcode id; unsigned type, bytes; };
    std::unordered_map<std::string,Var> variables_;
    std::unordered_map<std::string,std::vector<std::string>> children_;
    std::unordered_map<std::string,std::vector<SignalInfo>> signals_;
    std::unordered_set<fsdbVarIdcode> loaded_;
    std::vector<std::string> stack_;
    std::vector<bool> hidden_;
    std::vector<bool> unpacked_arrays_;
    FileInfo info_;
    std::string sdk_version_;

    static void* load(const std::string& path) {
        void* h=dlopen(path.c_str(),RTLD_NOW|RTLD_GLOBAL);
        if (!h) throw BackendError("READER_LOAD_FAILED", "dlopen " + path + ": " + dlerror());
        return h;
    }
    template<class T> T bind(const char* name) {
        dlerror(); void* address=dlsym(library_,name); const char* error=dlerror();
        if (error || !address) throw BackendError("READER_ABI_UNSUPPORTED",
            std::string("Missing reader entry ")+name+": "+(error ? error : "null"));
        return reinterpret_cast<T>(address);
    }
    void verify_source(void* handle, const char* symbol, const std::string& expected) {
        void* address=dlsym(handle,symbol);
        Dl_info origin{};
        if (!address || !dladdr(address,&origin) || !origin.dli_fname ||
            absolute_path(origin.dli_fname)!=absolute_path(expected))
            throw BackendError("READER_LOAD_FAILED", "Reader library source mismatch: " + expected);
    }
    static uint64_t time(const fsdbTag64& t) { return (uint64_t(t.H)<<32)|t.L; }
    static fsdbTag64 tag(int64_t t) {
        fsdbTag64 result{}; result.H=uint64_t(t)>>32; result.L=uint64_t(t); return result;
    }
    std::string parent() const { return stack_.empty() ? "" : stack_.back(); }
    static bool_T tree(fsdbTreeCBType type, void* client, void* data) {
        auto& b=*static_cast<FfrBackend*>(client);
        if (type==FSDB_TREE_CBT_SCOPE) {
            auto* scope=static_cast<fsdbTreeCBDataScope*>(data);
            std::string p=b.parent();
            std::string name=scope->name ? scope->name : "";
            std::string full=p.empty() ? name : p+"."+name;
            bool hidden=scope->is_hidden_scope || (!b.hidden_.empty() && b.hidden_.back());
            if (!hidden && !b.children_.count(full)) {
                b.children_[p].push_back(full); b.children_[full]; b.signals_[full];
            }
            b.stack_.push_back(full);
            b.hidden_.push_back(hidden);
        } else if (type==FSDB_TREE_CBT_UPSCOPE) {
            if (!b.stack_.empty()) b.stack_.pop_back();
            if (!b.hidden_.empty()) b.hidden_.pop_back();
        } else if (type==FSDB_TREE_CBT_ARRAY_BEGIN) {
            auto* array=static_cast<fsdbTreeCBDataArrayBegin*>(data);
            bool unpacked=!array->is_packed_array;
            if (unpacked && std::find(b.unpacked_arrays_.begin(),b.unpacked_arrays_.end(),true)==b.unpacked_arrays_.end() &&
                (b.hidden_.empty() || !b.hidden_.back())) {
                SignalInfo s; s.name=array->name ? array->name : ""; s.direction="none";
                auto bracket=s.name.rfind('[');
                if (bracket!=std::string::npos && sscanf(s.name.c_str()+bracket,"[%d:%d]",&s.left,&s.right)==2)
                    s.name.resize(bracket);
                s.full_name=b.parent().empty() ? s.name : b.parent()+"."+s.name;
                if (b.variables_.emplace(s.full_name,Var{s,0,FSDB_VT_MDA,FSDB_BYTES_PER_BIT_UNKNOWN}).second)
                    b.signals_[b.parent()].push_back(s);
            }
            b.unpacked_arrays_.push_back(unpacked);
        } else if (type==FSDB_TREE_CBT_ARRAY_END) {
            if (!b.unpacked_arrays_.empty()) b.unpacked_arrays_.pop_back();
        } else if (type==FSDB_TREE_CBT_VAR) {
            if (!b.hidden_.empty() && b.hidden_.back()) return 1;
            auto* v=static_cast<fsdbTreeCBDataVar*>(data);
            SignalInfo s;
            s.name=v->name ? v->name : "";
            if (!s.name.empty() && s.name[0]=='\\' && s.name.back()!=' ') s.name+=' ';
            // Reader 的普通总线名字包含声明范围，NPI 对外名字不含该后缀。
            std::string range="["+std::to_string(v->lbitnum)+":"+std::to_string(v->rbitnum)+"]";
            if (!s.name.empty() && s.name[0]!='\\' && s.name.size()>=range.size() &&
                s.name.compare(s.name.size()-range.size(),range.size(),range)==0)
                s.name.resize(s.name.size()-range.size());
            s.full_name=b.parent().empty() ? s.name : b.parent()+"."+s.name;
            s.left=v->lbitnum; s.right=v->rbitnum;
            s.direction=v->direction==FSDB_VD_INPUT ? "input" : v->direction==FSDB_VD_OUTPUT ? "output" :
                v->direction==FSDB_VD_INOUT ? "inout" : "none";
            if (b.variables_.emplace(s.full_name,Var{s,v->u.idcode,v->type,v->bytes_per_bit}).second &&
                std::find(b.unpacked_arrays_.begin(),b.unpacked_arrays_.end(),true)==b.unpacked_arrays_.end())
                b.signals_[b.parent()].push_back(s);
        }
        return 1;
    }
    struct Cursor {
        FfrBackend& backend;
        ffrVCIterOne* handle;
        Cursor(FfrBackend& b, const std::string& path) : backend(b), handle(nullptr) {
            auto it=b.variables_.find(path);
            if (it==b.variables_.end()) throw BackendError("SIGNAL_NOT_FOUND", "Signal '"+path+"' not found");
            const auto& v=it->second;
            unsigned t=v.type & 0x3f;
            if (v.bytes!=FSDB_BYTES_PER_BIT_1B || t==FSDB_VT_VCD_REAL || t==FSDB_VT_STRING ||
                t>=FSDB_VT_SV_VARIABLE)
                throw BackendError("UNSUPPORTED_SIGNAL_TYPE", "FFR currently supports four-state digital values: "+path);
            if (!b.loaded_.count(v.id)) {
                if (b.file_->ffrAddToSignalList(v.id)!=FSDB_RC_SUCCESS ||
                    b.file_->ffrLoadSignals()!=FSDB_RC_SUCCESS)
                    throw BackendError("FILE_READ_ERROR", "Failed to load signal: "+path);
                b.loaded_.insert(v.id);
            }
            handle=b.file_->ffrCreateVCTraverseHandle(v.id);
            if (!handle) throw BackendError("FILE_READ_ERROR", "Failed to create value cursor: "+path);
        }
        ~Cursor() { if (handle) backend.ops_.free(handle); }
        Cursor(const Cursor&)=delete;
        Cursor& operator=(const Cursor&)=delete;
    };
    Value current(Cursor& c, const std::string& radix) {
        fsdbTag64 t{}; byte_T* raw=nullptr;
        if (ops_.tag(c.handle,&t)!=FSDB_RC_SUCCESS || ops_.value(c.handle,&raw)!=FSDB_RC_SUCCESS || !raw)
            throw BackendError("FILE_READ_ERROR", "Cannot read current value");
        if (ops_.bytes(c.handle)!=FSDB_BYTES_PER_BIT_1B)
            throw BackendError("UNSUPPORTED_SIGNAL_TYPE", "Unsupported value encoding");
        std::string bin;
        for (unsigned i=0,n=ops_.bits(c.handle);i<n;++i) {
            if (raw[i]>=4) throw BackendError("UNSUPPORTED_SIGNAL_TYPE", "Unsupported logic value encoding");
            bin.push_back("01xz"[raw[i]]);
        }
        return {static_cast<int64_t>(time(t)),convert(bin,radix)};
    }
    static std::string convert(const std::string& bin, const std::string& radix) {
        if (radix=="bin") return bin;
        if (radix=="dec") {
            if (bin.find('x')!=std::string::npos)
                return bin.find_first_not_of('x')==std::string::npos ? "X" : "x";
            if (bin.find('z')!=std::string::npos)
                return bin.find_first_not_of('z')==std::string::npos ? "Z" : "z";
            std::string result="0";
            for (char bit:bin) {
                int carry=bit-'0';
                for (auto i=result.rbegin();i!=result.rend();++i) {
                    int v=(*i-'0')*2+carry; *i='0'+v%10; carry=v/10;
                }
                if (carry) result.insert(result.begin(),'0'+carry);
            }
            return result;
        }
        size_t group=radix=="oct" ? 3 : 4;
        std::string result;
        for (size_t end=bin.size();end;) {
            size_t begin=end>group ? end-group : 0;
            unsigned value=0; bool x=false,z=false,known=false;
            for (size_t i=begin;i<end;++i) {
                value=(value<<1)|(bin[i]=='1');
                x|=bin[i]=='x'; z|=bin[i]=='z'; known|=bin[i]=='0'||bin[i]=='1';
            }
            result.push_back(x ? (known||z ? 'x':'X') : z ? (known ? 'z':'Z') : "0123456789abcdef"[value]);
            end=begin;
        }
        std::reverse(result.begin(),result.end()); return result;
    }
public:
    explicit FfrBackend(const Session& session) {
        try {
            zlib_=load("libz.so.1");
            support_=load(session.support); library_=load(session.reader);
            auto version=bind<const char*(*)()>("_ZN9ffrObject13ffrGetVersionEv");
            sdk_version_=version();
            if (sdk_version_!="Verdi_T-2022.06-SP2" && sdk_version_!="Verdi_Y-2026.03-SP2")
                throw BackendError("READER_ABI_UNSUPPORTED", "Unvalidated FsdbReader build: "+sdk_version_);
            open_=bind<decltype(open_)>("_ZN9ffrObject8ffrOpen3EPc");
            file_info_=bind<decltype(file_info_)>("_ZN9ffrObject14ffrGetFSDBInfoEPcR11ffrFSDBInfo");
            ops_.first=bind<decltype(ops_.first)>("_ZN12ffrVCIterOne17ffrGotoTheFirstVCEv");
            ops_.next=bind<decltype(ops_.next)>("_ZN12ffrVCIterOne13ffrGotoNextVCEv");
            ops_.prev=bind<decltype(ops_.prev)>("_ZN12ffrVCIterOne13ffrGotoPrevVCEv");
            ops_.seek=bind<decltype(ops_.seek)>("_ZN12ffrVCIterOne11ffrGotoXTagEPvPi");
            ops_.tag=bind<decltype(ops_.tag)>("_ZN12ffrVCIterOne10ffrGetXTagEPv");
            ops_.value=bind<decltype(ops_.value)>("_ZN12ffrVCIterOne8ffrGetVCEPPh");
            ops_.bits=bind<decltype(ops_.bits)>("_ZN12ffrVCIterOne13ffrGetBitSizeEv");
            ops_.bytes=bind<decltype(ops_.bytes)>("_ZN12ffrVCIterOne17ffrGetBytesPerBitEv");
            ops_.free=bind<decltype(ops_.free)>("_ZN12ffrVCIterOne7ffrFreeEv");
            verify_source(library_,"_ZN9ffrObject8ffrOpen3EPc",session.reader);
            // 支持库以 link_map 检查实际映射路径。
            struct link_map* map=nullptr;
            if (dlinfo(support_,RTLD_DI_LINKMAP,&map)!=0 || !map ||
                absolute_path(map->l_name)!=absolute_path(session.support))
                throw BackendError("READER_LOAD_FAILED", "Support library source mismatch");
        } catch (...) { unload(); throw; }
    }
    ~FfrBackend() override { if (file_) file_->ffrClose(); unload(); }
    void unload() {
        if (library_) dlclose(library_);
        if (support_) dlclose(support_);
        if (zlib_) dlclose(zlib_);
        library_=support_=zlib_=nullptr;
    }
    const std::string& sdk_version() const { return sdk_version_; }
    void open(const std::string& path) override {
        ffrFSDBInfo fi{};
        if (file_info_(const_cast<char*>(path.c_str()),fi)!=FSDB_RC_SUCCESS)
            throw BackendError("FSDB_OPEN_FAILED", "Cannot read FSDB file information: "+path);
        file_=open_(const_cast<char*>(path.c_str()));
        if (!file_) throw BackendError("FSDB_OPEN_FAILED", "Failed to open FSDB: "+path);
        if (file_->ffrGetXTagType()!=FSDB_XTAG_TYPE_HL)
            throw BackendError("UNSUPPORTED_FILE_TYPE", "FFR requires integer simulation timestamps");
        fsdbTag64 first{},last{};
        if (file_->ffrGetMinFsdbTag64(&first)!=FSDB_RC_SUCCESS || file_->ffrGetMaxFsdbTag64(&last)!=FSDB_RC_SUCCESS)
            throw BackendError("FILE_READ_ERROR", "Cannot read time range");
        info_.min_time=time(first); info_.max_time=time(last);
        info_.scale=fi.scale_unit; info_.version.assign(fi.version,strnlen(fi.version,sizeof(fi.version)));
        info_.completed=fi.file_status==FSDB_FS_FINISHED;
        children_[""]; signals_[""];
        if (file_->ffrSetTreeCBFunc(tree,this)!=FSDB_RC_SUCCESS || file_->ffrReadScopeVarTree()!=FSDB_RC_SUCCESS)
            throw BackendError("FILE_READ_ERROR", "Cannot read scope/signal tree");
    }
    FileInfo info() override { return info_; }
    bool has_scope(const std::string& path) override { return children_.count(path); }
    std::vector<std::string> children(const std::string& path) override { return children_.at(path); }
    std::vector<SignalInfo> signals(const std::string& path) override { return signals_.at(path); }
    bool signal(const std::string& path, SignalInfo& result) override {
        auto it=variables_.find(path); if (it==variables_.end()) return false;
        result=it->second.info; return true;
    }
    Value point(const std::string& path, int64_t t, const std::string& radix) override {
        Cursor c(*this,path); auto xtag=tag(std::min(t,info_.max_time));
        if (ops_.seek(c.handle,&xtag,nullptr)!=FSDB_RC_SUCCESS)
            throw BackendError("FILE_READ_ERROR", "read failed");
        auto v=current(c,radix); v.time=t; return v;
    }
    Range range(const std::string& path, int64_t begin, int64_t end,
                const std::string& radix, size_t limit) override {
        Cursor c(*this,path); Range r; auto t=tag(std::min(begin,info_.max_time));
        if (ops_.seek(c.handle,&t,nullptr)!=FSDB_RC_SUCCESS) return r;
        Value v=current(c,radix);
        if (v.time>begin || v.time>end) return r;
        // NPI 范围输出以 begin 时的值开头，即使该时刻没有真实 VC。
        v.time=begin; r.total=1; if (limit) r.changes.push_back(v);
        while (ops_.next(c.handle)==FSDB_RC_SUCCESS) {
            v=current(c,radix); if (v.time>end) break;
            ++r.total; if (r.changes.size()<limit) r.changes.push_back(v);
        }
        return r;
    }
    Edge edge(const std::string& path, int64_t t, const std::string& kind, bool backward) override {
        Cursor c(*this,path); Edge e; auto xtag=tag(std::min(t,info_.max_time));
        if (ops_.seek(c.handle,&xtag,nullptr)!=FSDB_RC_SUCCESS) return e;
        if (kind=="any") {
            if ((backward ? ops_.prev(c.handle) : ops_.next(c.handle))==FSDB_RC_SUCCESS) {
                e.found=true; e.value=current(c,"bin");
            }
            return e;
        }
        const std::string target=kind=="rising" ? "1" : "0";
        if (!backward && t==std::numeric_limits<int64_t>::max()) return e;
        auto matches=[&](const Value& v) { return v.value==target; };
        if (backward) {
            do {
                auto v=current(c,"bin");
                if (v.time<t && matches(v)) { e.found=true; e.value={v.time,target}; return e; }
            } while (ops_.prev(c.handle)==FSDB_RC_SUCCESS);
        } else {
            while (ops_.next(c.handle)==FSDB_RC_SUCCESS) {
                auto v=current(c,"bin");
                // NPI 调用从 t+1 开始，find 本身排除该起点。
                if (v.time>t+1 && matches(v)) { e.found=true; e.value={v.time,target}; return e; }
            }
        }
        return e;
    }
    int64_t count(const std::string& path, int64_t begin, int64_t end) override {
        Cursor c(*this,path); auto t=tag(begin); int64_t n=0;
        if (ops_.seek(c.handle,&t,nullptr)!=FSDB_RC_SUCCESS) return 0;
        do {
            fsdbTag64 current_tag{};
            if (ops_.tag(c.handle,&current_tag)!=FSDB_RC_SUCCESS) break;
            auto v=static_cast<int64_t>(time(current_tag));
            if (v>end) break;
            if (v>=begin) ++n;
        } while (ops_.next(c.handle)==FSDB_RC_SUCCESS);
        return n;
    }
};
} // namespace wave
#endif
