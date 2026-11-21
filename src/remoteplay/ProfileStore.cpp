// SPDX-License-Identifier: GPL-3.0-only
// macOS/POSIX port of Veyra's profile store (same record layout as Windows, without DPAPI).
#include "veyra/remoteplay/ProfileStore.h"
#include <QStandardPaths>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <random>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>
namespace veyra::remoteplay {
std::filesystem::path profileDirectory(){
    const QString base=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    std::filesystem::path dir=std::filesystem::path(base.toStdString())/"remoteplay";
    std::error_code ec;std::filesystem::create_directories(dir,ec);
    ::chmod(dir.c_str(),0700);
    return dir;
}
namespace {
struct SecretBytes {std::vector<uint8_t> data;~SecretBytes(){volatile uint8_t* p=data.data();for(size_t i=0;i<data.size();++i)p[i]=0;}};
void append32(std::vector<uint8_t>& b,uint32_t n){for(unsigned i=0;i<4;++i)b.push_back(uint8_t(n>>(8*i)));}
uint32_t read32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
}
bool saveProfile(const std::filesystem::path& path,const NativeConnectRequest& request){
    if(!validHost(request.host)||request.video.validate())return false;
    if(!request.consoleId.empty()&&(request.consoleId.size()!=12||!std::all_of(request.consoleId.begin(),request.consoleId.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');})))return false;
    SecretBytes plain;
    auto& b=plain.data;b.insert(b.end(),{'V','R','P',2});
    append32(b,uint32_t(request.host.size()));append32(b,request.video.width);append32(b,request.video.height);
    append32(b,request.video.fps);append32(b,request.video.bitrateKbps);append32(b,uint32_t(request.video.codec));
    b.insert(b.end(),request.credentials.accountId.begin(),request.credentials.accountId.end());
    b.insert(b.end(),request.credentials.registrationKey.begin(),request.credentials.registrationKey.end());
    b.insert(b.end(),request.credentials.sessionKey.begin(),request.credentials.sessionKey.end());
    b.insert(b.end(),request.host.begin(),request.host.end());
    b.push_back(request.viewOnly?1:0);
    b.push_back(uint8_t(request.consoleId.size()));
    b.insert(b.end(),request.consoleId.begin(),request.consoleId.end());
    std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);if(ec)return false;
    std::random_device rd;
    auto temp=path;temp+="."+std::to_string(::getpid())+"."+std::to_string(rd())+".tmp";
    const int fd=::open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0)return false;
    size_t done=0;bool ok=true;
    while(done<b.size()){const auto n=::write(fd,b.data()+done,b.size()-done);if(n<=0){ok=false;break;}done+=size_t(n);}
    if(ok)ok=::fsync(fd)==0;
    ::close(fd);
    if(ok)ok=::rename(temp.c_str(),path.c_str())==0;
    if(!ok)::unlink(temp.c_str());
    return ok;
}
std::optional<NativeConnectRequest> loadProfile(const std::filesystem::path& path,ProfileLoadError* error){
    if(error)*error=ProfileLoadError::Read;
    std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f){std::error_code ec;if(error&&!std::filesystem::exists(path,ec)&&!ec)*error=ProfileLoadError::Missing;return {};}
    const auto size=f.tellg();if(size<=0||size>8192)return {};
    SecretBytes plain;plain.data.resize(static_cast<size_t>(size));f.seekg(0);
    if(!f.read(reinterpret_cast<char*>(plain.data.data()),size))return {};
    if(error)*error=ProfileLoadError::Format;
    const auto* p=plain.data.data();const auto n=plain.data.size();
    constexpr size_t header=28,credentials=40;
    if(n<header+credentials||std::memcmp(p,"VRP",3)!=0||(p[3]!=1&&p[3]!=2))return {};
    const auto hostSize=read32(p+4);
    const size_t base=header+credentials+hostSize;
    if(hostSize>253||n<base||read32(p+24)>2)return {};
    if(p[3]==1&&n!=base)return {};
    if(p[3]==2&&(n<base+2||p[base]>1||(p[base+1]!=0&&p[base+1]!=12)||n!=base+2+p[base+1]))return {};
    NativeConnectRequest r;r.video.width=read32(p+8);r.video.height=read32(p+12);r.video.fps=read32(p+16);
    r.video.bitrateKbps=read32(p+20);r.video.codec=Codec(read32(p+24));
    std::memcpy(r.credentials.accountId.data(),p+header,8);std::memcpy(r.credentials.registrationKey.data(),p+header+8,16);
    std::memcpy(r.credentials.sessionKey.data(),p+header+24,16);
    r.host.assign(reinterpret_cast<const char*>(p+header+credentials),hostSize);
    if(p[3]==2){
        r.viewOnly=p[base]!=0;r.consoleId.assign(reinterpret_cast<const char*>(p+base+2),p[base+1]);
        if(!std::all_of(r.consoleId.begin(),r.consoleId.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}))return {};
    }
    if(!validHost(r.host)||r.video.validate())return {};
    if(error)*error=ProfileLoadError::None;return r;
}
}
