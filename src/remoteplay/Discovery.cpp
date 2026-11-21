// SPDX-License-Identifier: GPL-3.0-only
// POSIX port of Veyra's PS5 discovery: directed broadcasts on every active IPv4 LAN
// (getifaddrs instead of GetAdaptersAddresses), then chiaki's discovery service.
#include "veyra/remoteplay/Discovery.h"
#include <chiaki/discoveryservice.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <string>
namespace veyra::remoteplay {
namespace {
struct DiscoveryResult {
    std::mutex mutex;DiscoveryReport report;
    static void logging(ChiakiLogLevel level,const char* message,void* user) noexcept {
        try {auto& out=*static_cast<DiscoveryResult*>(user);std::lock_guard lock(out.mutex);
            if(level==CHIAKI_LOG_ERROR)out.report.error=-1402;
            if(out.report.diagnostics.size()<256)out.report.diagnostics.emplace_back(message?message:"");
        }catch(...){}
    }
    static void receive(ChiakiDiscoveryHost* hosts,size_t count,void* user) noexcept {
        auto& out=*static_cast<DiscoveryResult*>(user);
        try{std::lock_guard lock(out.mutex);out.report.hosts.clear();
            for(size_t i=0;i<std::min(count,size_t(16));++i)
                if(chiaki_discovery_host_is_ps5(&hosts[i])&&hosts[i].host_addr&&validHost(hosts[i].host_addr))
                    out.report.hosts.push_back({hosts[i].host_addr,hosts[i].state==CHIAKI_DISCOVERY_HOST_STATE_STANDBY,hosts[i].host_id?hosts[i].host_id:""});
        }catch(...){}
    }
};
void quiet(ChiakiLogLevel,const char*,void*){}
int prefixLength(uint32_t mask){int n=0;while(mask&0x80000000u){++n;mask<<=1;}return n;}
}
DiscoveryReport discoverLocalPs5(std::stop_token stop){
    if(stop.stop_requested())return {};
    auto initialized=initializeChiaki();
    if(!initialized.ok)return {{},{"chiaki_lib_init failed"},initialized.code};
    DiscoveryResult result;ChiakiLog log{};
    chiaki_log_init(&log,CHIAKI_LOG_ERROR|CHIAKI_LOG_WARNING|CHIAKI_LOG_INFO,DiscoveryResult::logging,&result);
    std::vector<sockaddr_storage> broadcasts;
    ifaddrs* list=nullptr;
    if(getifaddrs(&list)!=0)return {{},{"getifaddrs failed"},-1400};
    for(ifaddrs* it=list;it;it=it->ifa_next){
        if(!it->ifa_addr||it->ifa_addr->sa_family!=AF_INET||!it->ifa_netmask)continue;
        if(!(it->ifa_flags&IFF_UP)||!(it->ifa_flags&IFF_RUNNING)||(it->ifa_flags&IFF_LOOPBACK)||(it->ifa_flags&IFF_POINTOPOINT))continue;
        const uint32_t address=ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr);
        const uint32_t mask=ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_netmask)->sin_addr.s_addr);
        const int prefix=prefixLength(mask);
        if(prefix==0||prefix>30)continue;
        if((address>>24)==127||(address>>16)==0xa9fe||address==0)continue;
        sockaddr_storage target{};auto* ipv4=reinterpret_cast<sockaddr_in*>(&target);
#ifdef __APPLE__
        ipv4->sin_len=sizeof(sockaddr_in);
#endif
        ipv4->sin_family=AF_INET;ipv4->sin_addr.s_addr=htonl(address|~mask);
        if(std::any_of(broadcasts.begin(),broadcasts.end(),[&](const auto& old){return reinterpret_cast<const sockaddr_in*>(&old)->sin_addr.s_addr==ipv4->sin_addr.s_addr;}))continue;
        char printable[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&ipv4->sin_addr,printable,sizeof(printable));
        result.report.diagnostics.push_back(std::string("interface=")+it->ifa_name+" directed_broadcast="+printable);
        broadcasts.push_back(target);
    }
    freeifaddrs(list);
    if(broadcasts.empty())return {{},{"No active IPv4 LAN broadcast targets"},-1401};
    sockaddr_storage address{};auto* ipv4=reinterpret_cast<sockaddr_in*>(&address);
#ifdef __APPLE__
    ipv4->sin_len=sizeof(sockaddr_in);
#endif
    ipv4->sin_family=AF_INET;ipv4->sin_addr.s_addr=INADDR_BROADCAST;ipv4->sin_port=htons(CHIAKI_DISCOVERY_PORT_PS5);
    ChiakiDiscoveryServiceOptions options{};options.hosts_max=16;options.host_drop_pings=20;options.ping_ms=500;options.ping_initial_ms=0;
    options.send_addr=&address;options.send_addr_size=sizeof(sockaddr_in);options.cb=DiscoveryResult::receive;options.cb_user=&result;
    options.broadcast_addrs=broadcasts.data();options.broadcast_num=broadcasts.size();
    ChiakiDiscoveryService service{};
    const auto started=chiaki_discovery_service_init(&service,&options,&log);
    if(started!=CHIAKI_ERR_SUCCESS){result.report.error=int(started);result.report.diagnostics.push_back("discovery_init failed code="+std::to_string(int(started)));return std::move(result.report);}
    std::mutex mutex;std::condition_variable_any wait;std::unique_lock lock(mutex);
    wait.wait_for(lock,stop,std::chrono::seconds(6),[]{return false;});
    chiaki_discovery_service_fini(&service);
    std::lock_guard guard(result.mutex);
    result.report.diagnostics.push_back("discovery finished hosts="+std::to_string(result.report.hosts.size())+" cancelled="+(stop.stop_requested()?"1":"0"));
    return std::move(result.report);
}
BackendResult wakeLocalPs5(const NativeConnectRequest& request){
    if(!validHost(request.host))return {false,-1300,"invalid_wakeup_host"};
    auto init=initializeChiaki();if(!init.ok)return init;
    const auto& key=request.credentials.registrationKey;
    const auto end=std::find(key.begin(),key.end(),'\0');uint64_t credential=0;
    const auto parsed=std::from_chars(key.data(),key.data()+(end-key.begin()),credential,16);
    if(parsed.ec!=std::errc{}||parsed.ptr!=key.data()+(end-key.begin()))return {false,-1301,"invalid_wakeup_credential"};
    ChiakiLog log{};chiaki_log_init(&log,0,quiet,nullptr);
    const auto code=chiaki_discovery_wakeup(&log,nullptr,request.host.c_str(),credential,true);
    return {code==CHIAKI_ERR_SUCCESS,int(code),"chiaki_discovery_wakeup"};
}
}
