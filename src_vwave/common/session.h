#ifndef WAVE_SESSION_H
#define WAVE_SESSION_H
#include "common/json_parser.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wave {
inline std::string absolute_path(const std::string& path) {
    char result[PATH_MAX];
    if (realpath(path.c_str(), result)) return result;
    if (!path.empty() && path.front() == '/') return path;
    if (!getcwd(result, sizeof(result))) throw std::runtime_error("getcwd failed");
    return std::string(result) + "/" + path;
}

// 用于会话的库内容指纹，不引入 OpenSSL 或额外外部命令。
inline std::string sha256_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read library: " + path);
    static const uint32_t k[] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<uint32_t,8> h{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                            0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    auto rotate = [](uint32_t x, unsigned n) { return (x >> n) | (x << (32-n)); };
    auto block = [&](const unsigned char* p) {
        uint32_t w[64];
        for (int i=0;i<16;++i) w[i]=(uint32_t(p[4*i])<<24)|(uint32_t(p[4*i+1])<<16)|
            (uint32_t(p[4*i+2])<<8)|uint32_t(p[4*i+3]);
        for (int i=16;i<64;++i) {
            uint32_t a=w[i-15], b=w[i-2];
            w[i]=w[i-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+w[i-7]+(rotate(b,17)^rotate(b,19)^(b>>10));
        }
        auto s=h;
        for (int i=0;i<64;++i) {
            uint32_t t1=s[7]+(rotate(s[4],6)^rotate(s[4],11)^rotate(s[4],25))+
                ((s[4]&s[5])^(~s[4]&s[6]))+k[i]+w[i];
            uint32_t t2=(rotate(s[0],2)^rotate(s[0],13)^rotate(s[0],22))+
                ((s[0]&s[1])^(s[0]&s[2])^(s[1]&s[2]));
            s={{t1+t2,s[0],s[1],s[2],s[3]+t1,s[4],s[5],s[6]}};
        }
        for (int i=0;i<8;++i) h[i]+=s[i];
    };
    unsigned char buffer[128]{};
    uint64_t size=0;
    size_t tail=0;
    while (input) {
        input.read(reinterpret_cast<char*>(buffer),64);
        tail=static_cast<size_t>(input.gcount()); size+=tail;
        if (tail==64) block(buffer); else break;
    }
    if (input.bad()) throw std::runtime_error("Library read failed: " + path);
    buffer[tail++]=0x80;
    size_t padded=tail<=56 ? 64 : 128;
    std::fill(buffer+tail,buffer+padded,0);
    for (int i=0;i<8;++i) buffer[padded-1-i]=static_cast<unsigned char>((size*8)>>(8*i));
    block(buffer); if (padded==128) block(buffer+64);
    std::ostringstream out; out<<std::hex<<std::setfill('0');
    for (auto x:h) out<<std::setw(8)<<x;
    return out.str();
}
inline std::string file_stamp(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st)) throw std::runtime_error("Cannot stat: " + path);
    std::ostringstream out;
    out << st.st_dev << ':' << st.st_ino << ':' << st.st_size << ':'
        << st.st_mtim.tv_sec << ':' << st.st_mtim.tv_nsec << ':'
        << st.st_ctim.tv_sec << ':' << st.st_ctim.tv_nsec;
    return out.str();
}
struct Session {
    std::string backend, sdk_home, adapter = "vwave-backend-v1";
    std::string file, file_identity, reader, support, reader_hash, support_hash;
    std::string json() const {
        JsonObject j;
        j.set("schema_version", int64_t(1)); j.set("backend", backend);
        j.set("sdk_home", sdk_home); j.set("adapter", adapter);
        j.set("file", file); j.set("file_identity", file_identity);
        j.set("reader_library", reader); j.set("support_library", support);
        j.set("reader_sha256", reader_hash); j.set("support_sha256", support_hash);
        return j.dump();
    }
    static Session parse(const std::string& text) {
        JsonParser j;
        if (!j.parse(text) || j.get_int("schema_version",0)!=1)
            throw std::runtime_error("Invalid session metadata");
        Session s;
        s.backend=j.get_string("backend"); s.sdk_home=j.get_string("sdk_home");
        s.adapter=j.get_string("adapter"); s.file=j.get_string("file");
        s.file_identity=j.get_string("file_identity"); s.reader=j.get_string("reader_library");
        s.support=j.get_string("support_library"); s.reader_hash=j.get_string("reader_sha256");
        s.support_hash=j.get_string("support_sha256");
        return s;
    }
    void write(const std::string& dir) const {
        const std::string temp=dir+"/session.json.tmp";
        std::ofstream f(temp);
        if (!f || !(f<<json()<<'\n')) throw std::runtime_error("Cannot write session metadata");
        f.close();
        if (!f || rename(temp.c_str(),(dir+"/session.json").c_str()))
            throw std::runtime_error("Cannot publish session metadata");
    }
};
} // namespace wave
#endif
