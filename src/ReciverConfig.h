#pragma once
#include "audio_helper.h"

class ReciverConfig{
private:
    Preferences prefs_;
    std :: mutex mu_;
    IPAddress ip_;
    uint16_t port_;
public:
    ReciverConfig(){}
    ~ReciverConfig(){
        prefs_.end();
    }

    void begin()
    {
        prefs_.begin(PREF_NAMESPACE,true);
        load();
        prefs_.end();
    }
    void load()
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(PREF_NAMESPACE,true);
        String saved_ip = prefs_.getString("pc_ip","");
        String  saved_port = prefs_.getString("pc_port","");
        prefs_.end();
        IPAddress tmp;
        if (saved_ip.length()&& tmp.fromString(saved_ip))
        {
            ip_ = tmp;

        }
        else{
            ip_ = IPAddress(0,0,0,0);
        }
        if (saved_port.length())
        {
            long p = saved_port.toInt();
            port_ = (p>0 && p <= 65535) ? (uint16_t)p: 0;
        }
        else{
            port_ = 0;
        }
    }
    void save(const char* ip_str, uint16_t port)
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(PREF_NAMESPACE,false);
        prefs_.putString("pc_ip",String(ip_str));
        prefs_.putString("pc_port",String((unsigned)port));
        prefs_.end();
        IPAddress tmp;
        if (ip_str && ip_str[0] && tmp.fromString(String(ip_str)))
        {
            ip_ = tmp;
            port_ = port;
        }
    }
    void clear()
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(PREF_NAMESPACE,false);
        prefs_.remove("pc_ip");
        prefs_.remove("pc_port");
        prefs_.end();
        ip_ = IPAddress(0,0,0,0);
        port_ = 0;
    }
    void get(IPAddress &out_ip, uint16_t &out_port)
    {
        std::lock_guard<std::mutex> lock(mu_);
        out_ip = ip_;
        out_port = port_;
    }   
    String ipString()
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (ip_ == IPAddress(0,0,0,0))
        {
            return String("(none)");
        }
        return ip_.toString();
        
    }
    unsigned short port()
    {
        std::lock_guard<std::mutex> lock(mu_);
        return port_;
    }
    bool isvalid()
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (ip_ != IPAddress(0,0,0,0)&& port_ != 0)
        {
            return true;
        }
        return false;
        
    }

};
