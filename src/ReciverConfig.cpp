#include "headers/ReciverConfig.h"
#include <Client.h> // base class for WiFiClient


ReciverConfig::ReciverConfig()
{}

ReciverConfig::~ReciverConfig()
{}

void ReciverConfig::begin()
{
    load();
}

void ReciverConfig::load()
{
    std::lock_guard<std::mutex> lock(mu_);
    prefs_.begin(PREF_NAMESPACE,true);
    String saved_ip = prefs_.getString("pc_ip","");
    String saved_port = prefs_.getString("pc_port","");
    prefs_.end();

    IPAddress tmp;
    if (saved_ip.length() && tmp.fromString(saved_ip))
    {
        ip_ = tmp;
    }
    else
    {
        ip_ = IPAddress(0,0,0,0);
    }
    if (saved_ip.length())
    {
        long p = saved_port.toInt();
        port_ = (p > 0 && p <= 65535) ? static_cast<uint16_t>(p) : 0;
    }
    else
    {
        port_ = 0;
    }
}

void ReciverConfig::save(const char* ip_str, uint16_t port) 
{
    std::lock_guard<std::mutex> lock(mu_);
    prefs_.begin(PREF_NAMESPACE,false);
    prefs_.putString("pc_ip", String(ip_str));
    prefs_.putString("pc_port", String((unsigned)port));
    prefs_.end();
    IPAddress tmp;
    if (ip_str && ip_str[0] && tmp.fromString(String(ip_str)))
    {
        ip_ = tmp;
        port_ = port;
    }
}

void ReciverConfig::clear()
{
    std::lock_guard<std::mutex> lock(mu_);
    prefs_.begin(PREF_NAMESPACE,false);
    prefs_.remove("pc_ip");
    prefs_.remove("pc_port");
    prefs_.end();


    ip_ = IPAddress(0,0,0,0);
    port_ = 0;
}

void ReciverConfig::get(IPAddress &out_ip, uint16_t &out_port)
{
    std::lock_guard<std::mutex> lock(mu_);
    out_ip = ip_;
    out_port = port_;
}

String ReciverConfig::ipString()
{
    std::lock_guard<std::mutex> lock(mu_);
    if (ip_ == IPAddress(0,0,0,0))
    {
        return String("None");
    }
    return ip_.toString();
}

unsigned short ReciverConfig::port()
{
    std::lock_guard<std::mutex> lock(mu_);
    return port_;
}

bool ReciverConfig::isValid()
{
    std::lock_guard<std::mutex> lock(mu_);
    return (ip_ != IPAddress(0,0,0,0) && port_ != 0);
}

//has to be rewritten
bool ReciverConfig::ConnectTOReciverIP(WiFiClient* tcpClient)
{
    if (tcpClient == nullptr) return false;

    // copy ip/port under lock, then release lock before blocking connect()
    IPAddress ip_copy;
    uint16_t port_copy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ip_copy = ip_;
        port_copy = port_;
    }

    if (ip_copy == IPAddress(0,0,0,0) || port_copy == 0) {
        return false;
    }

    // ensure previous connection closed
    tcpClient->stop();
    delay(10); // small gap to allow socket close - keep short

    // try connect
    bool con_ok = false;
    // Client::connect returns int for some implementations; treat non-zero as success
    con_ok = (tcpClient->connect(ip_copy, port_copy) ? true : false);

    if (!con_ok || !tcpClient->connected())
    {
        tcpClient->stop();
        Serial.println("RECIVER: CONNECTION failed");
        return false;
    }

    // disable Nagle if supported
    tcpClient->setNoDelay(true);

    Serial.print("RECIVER: connected to ip: ");
    Serial.print(ip_copy.toString());
    Serial.print(":");
    Serial.println(port_copy);

    return true;
}

