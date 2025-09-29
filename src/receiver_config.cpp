#include "receiver_config.h"


ReciverConfig :: ReciverConfig()
    :ip_(0,0,0,0),port_(0)
{
    
}

ReciverConfig:: ~ReciverConfig()
{
    prefs_.end();
}

void ReciverConfig::begin()
{
    std::lock_guard<std::mutex>lock(mu_);
    prefs_.begin(PREF_NAMESPACE,true);
    load();
    prefs_.end();
}

void ReciverConfig::load()
{
    std:: lock_guard<std::mutex> lock(mu_);
    prefs_.begin(PREF_NAMESPACE,true);
    String saved_ip = prefs_.getString("pc_ip","");
    String saved_port = prefs_.getString("pc_port","");
    prefs_.end();
    IPAddress tmp;
    if (saved_ip.length()&&tmp.fromString(saved_ip))
    {
        ip_ = tmp;
    }
    else
    {
        ip_ = IPAddress(0,0,0,0);
    }

    if (saved_port.length())
    {
        long p = saved_port.toInt();
        port_ = (p > 0 && p<= 65535) ? (uint16_t)p : 0;
    }
    else
    {
        port_ = 0;
    } 
}

void ReciverConfig::save(const char* ip_str, uint16_t port)
{
    std:: lock_guard <std::mutex> lock(mu_);
    prefs_.begin(PREF_NAMESPACE,false);
    prefs_.putString("pc_ip",String(ip_str));
    prefs_.putString("pc_port",String((unsigned)port));
    prefs_.end();
    IPAddress tmp;
    if (ip_str && ip_str[0]&&tmp.fromString(String(ip_str)))
    {
        ip_ = tmp;
        port_ = port;
    }
    else
    {
        ip_ = IPAddress(0,0,0,0);
        port = 0;
    }
}

void ReciverConfig::clear()
{
    std::lock_guard<std::mutex>lock(mu_);
    prefs_.begin(PREF_NAMESPACE,false);
    prefs_.remove("pc_ip");
    prefs_.remove("pc_port");
    prefs_.end();
    ip_ = IPAddress(0,0,0,0);
    port_ =0 ;
}

void ReciverConfig::get(IPAddress &out_ip,uint16_t & out_port)
{
    std::lock_guard<std::mutex>lock(mu_);
    out_ip = ip_;
    out_port = port_;
}

String ReciverConfig::ipString()const{
    std::lock_guard<std::mutex>lock(mu_);
    return (ip_ == IPAddress(0,0,0,0)) ? String("(none)") : ip_.toString();
}

unsigned short ReciverConfig::port() const{
    std::lock_guard <std::mutex>lock(mu_);
    return port_;
}
bool ReciverConfig::isvalid() const{
    std::lock_guard<std::mutex>lock(mu_);
    return (ip_ != IPAddress(0,0,0,0)&& port_ != 0);
}
