#ifndef WAVE_HANDLERS_H
#define WAVE_HANDLERS_H
#include "backend/backend.h"
#include "common/json_parser.h"
#include "common/protocol.h"
#include <sstream>
#include <unistd.h>

namespace wave {
namespace server {

inline void add_source(JsonObject& data) {
    data.set("backend", g_session.backend);
    data.set("sdk_home", g_session.sdk_home);
    data.set("adapter", g_session.adapter);
    data.set("reader_library", g_session.reader);
    data.set("support_library", g_session.support);
    data.set("reader_sha256", g_session.reader_hash);
    data.set("support_sha256", g_session.support_hash);
}
inline SignalInfo require_signal(const std::string& path) {
    SignalInfo s;
    if (!g_backend->signal(path,s)) throw BackendError(err::SIGNAL_NOT_FOUND,"Signal '"+path+"' not found");
    return s;
}
inline JsonObject describe_signal(const SignalInfo& s) {
    JsonObject j;
    j.set("name",s.name); j.set("full_name",s.full_name);
    j.set("left",int64_t(s.left)); j.set("right",int64_t(s.right));
    j.set("direction",s.direction); return j;
}
static std::string handle_status(int id) {
    JsonObject data;
    data.set("fsdb_file",g_fsdb_path);
    data.set("uptime_seconds",std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now()-g_start_time).count());
    data.set("pid",int64_t(getpid())); add_source(data);
    return make_ok_response(id,data.dump());
}
static std::string handle_file_info(int id) {
    auto i=g_backend->info(); JsonObject data;
    data.set("file",g_fsdb_path); data.set("min_time",i.min_time); data.set("max_time",i.max_time);
    data.set("scale_unit",i.scale); data.set("version",i.version); data.set("is_completed",int64_t(i.completed));
    add_source(data); return make_ok_response(id,data.dump());
}
static std::string handle_list_scopes(int id,const JsonParser& params) {
    auto path=params.get_string("path");
    int depth=std::max<int64_t>(1,std::min<int64_t>(10,params.get_int("depth",1)));
    auto scopes=g_backend->scopes(path,depth);
    if (params.get_bool("compact",false) && !path.empty()) {
        auto prefix=path+".";
        for (auto& s:scopes) if (s.compare(0,prefix.size(),prefix)==0) s=s.substr(prefix.size());
    }
    JsonObject data; data.set("path",path.empty()?"/":path); data.set_array("scopes",scopes);
    data.set("count",int64_t(scopes.size()));
    if (scopes.size()>=500) data.set("truncated",int64_t(1));
    return make_ok_response(id,data.dump());
}
static std::string handle_list_signals(int id,const JsonParser& params) {
    auto path=params.get_string("path");
    if (path.empty()) throw BackendError(err::INVALID_PARAMS,"Missing 'path' parameter");
    if (!g_backend->has_scope(path)) throw BackendError(err::SCOPE_NOT_FOUND,"Scope '"+path+"' not found");
    auto signals=g_backend->signals(path); JsonObject data; data.set("path",path);
    if (params.get_bool("compact",false)) {
        std::vector<std::string> result;
        for (const auto& s:signals) {
            std::ostringstream entry; entry<<leaf_name(s.full_name);
            if (s.left!=0 || s.right!=0) entry<<'['<<s.left<<':'<<s.right<<']';
            if (s.direction=="input") entry<<" i";
            else if (s.direction=="output") entry<<" o";
            else if (s.direction=="inout") entry<<" io";
            result.push_back(entry.str());
        }
        data.set_array("signals",result); data.set("count",int64_t(result.size()));
    } else {
        std::ostringstream arr; arr<<'[';
        for (size_t i=0;i<signals.size();++i) { if (i) arr<<','; arr<<describe_signal(signals[i]).dump(); }
        arr<<']'; data.set_raw("signals",arr.str());
    }
    return make_ok_response(id,data.dump());
}
static std::string handle_signal_info(int id,const JsonParser& params) {
    auto path=params.get_string("signal");
    if (path.empty()) throw BackendError(err::INVALID_PARAMS,"Missing 'signal' parameter");
    return make_ok_response(id,describe_signal(require_signal(path)).dump());
}
static std::string handle_get_value_at(int id,const JsonParser& params) {
    auto time=params.get_int("time",-1);
    if (time<0) throw BackendError(err::INVALID_TIME,"Missing or invalid 'time'");
    auto signals=params.get_array("signals");
    if (signals.empty()) {
        auto s=params.get_string("signal"); if (!s.empty()) signals.push_back(s);
    }
    if (signals.empty()) throw BackendError(err::INVALID_PARAMS,"No signals specified");
    auto radix=params.get_string("radix","bin");
    std::ostringstream arr; arr<<'[';
    for (size_t i=0;i<signals.size();++i) {
        if (i) arr<<',';
        JsonObject j; j.set("signal",signals[i]); SignalInfo s;
        if (!g_backend->signal(signals[i],s)) j.set("error","not found");
        else {
            try {
                auto v=g_backend->point(signals[i],time,radix);
                j.set("value",v.value); j.set("actual_time",v.time);
            } catch (const BackendError& e) {
                j.set("value","x"); j.set("error",std::string(e.what()));
                j.set("error_code",e.code); j.set("actual_time",time);
            }
        }
        arr<<j.dump();
    }
    arr<<']'; JsonObject data; data.set("time",time); data.set_raw("values",arr.str());
    return make_ok_response(id,data.dump());
}
static std::string handle_get_value_between(int id,const JsonParser& params) {
    auto signal=params.get_string("signal");
    if (signal.empty()) throw BackendError(err::INVALID_PARAMS,"Missing 'signal'");
    auto begin=params.get_int("begin",-1), end=params.get_int("end",-1);
    if (begin<0 || end<begin) throw BackendError(err::INVALID_TIME,"Invalid time range");
    require_signal(signal);
    auto limit=std::max<int64_t>(1,std::min<int64_t>(100000,params.get_int("limit",1000)));
    auto r=g_backend->range(signal,begin,end,params.get_string("radix","bin"),limit);
    std::ostringstream arr; arr<<'[';
    for (size_t i=0;i<r.changes.size();++i) {
        if (i) arr<<',';
        JsonObject v; v.set("time",r.changes[i].time); v.set("value",r.changes[i].value); arr<<v.dump();
    }
    arr<<']'; JsonObject data;
    data.set("signal",signal); data.set("begin",begin); data.set("end",end);
    data.set("total_changes",r.total); data.set_raw("changes",arr.str());
    if (r.total>limit) data.set("truncated",int64_t(1));
    return make_ok_response(id,data.dump());
}
static std::string handle_find_signals(int id,const JsonParser& params) {
    auto pattern=params.get_string("pattern"), scope=params.get_string("scope");
    if (pattern.empty()) throw BackendError(err::INVALID_PARAMS,"Missing 'pattern'");
    auto result=g_backend->find(scope,pattern); JsonObject data;
    data.set("pattern",pattern); if (!scope.empty()) data.set("scope",scope);
    data.set_array("signals",result); data.set("count",int64_t(result.size()));
    if (result.size()>=200) data.set("truncated",int64_t(1));
    return make_ok_response(id,data.dump());
}
static std::string handle_next_edge(int id,const JsonParser& params) {
    auto signal=params.get_string("signal"); auto time=params.get_int("time",-1);
    if (signal.empty()) throw BackendError(err::INVALID_PARAMS,"Missing 'signal'");
    if (time<0) throw BackendError(err::INVALID_TIME,"Missing 'time'");
    require_signal(signal);
    auto e=g_backend->edge(signal,time,params.get_string("edge","any"),params.get_string("dir","forward")=="backward");
    JsonObject data; data.set("signal",signal); data.set("from_time",time);
    data.set("edge",params.get_string("edge","any")); data.set("dir",params.get_string("dir","forward"));
    data.set("found_time",e.found ? e.value.time : int64_t(-1));
    if (e.found) data.set("value",e.value.value);
    return make_ok_response(id,data.dump());
}
static std::string handle_vc_count(int id,const JsonParser& params) {
    auto signal=params.get_string("signal");
    if (signal.empty()) throw BackendError(err::INVALID_PARAMS,"Missing 'signal'");
    auto begin=params.get_int("begin",0),end=params.get_int("end",-1);
    if (end<0) end=g_backend->info().max_time;
    require_signal(signal);
    JsonObject data; data.set("signal",signal); data.set("begin",begin); data.set("end",end);
    data.set("count",g_backend->count(signal,begin,end));
    return make_ok_response(id,data.dump());
}
} // namespace server
} // namespace wave
#endif
