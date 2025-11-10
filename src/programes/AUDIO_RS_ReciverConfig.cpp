#include "headers/ReciverConfig.h"


TaskHandle_t ReciverConfig::ConfSRButtonTaskHandle_ = nullptr;

ReciverConfig::ReciverConfig(const char* prefs_namespace)
    :prefs_namespace_(prefs_namespace)
{
}

ReciverConfig::~ReciverConfig()
{
}



void ReciverConfig::setAudioRsPtr(AUDIO_RS* p)
{
    audio_rs_class_ptr_ = p;
}

void ReciverConfig::begin()
{
    // first load any previously saved values
    load();

    // If we already have a valid receiver configuration, nothing to do.
    // If you want to always show the portal regardless, remove the isValid() check.
    if (isValid()) {
        Serial.printf("ReciverConfig::begin() - already configured: %s:%u\n",
                      ip_.toString().c_str(), (unsigned)port_);
        return;
    }

    // If not valid, launch WiFiManager captive portal to configure WiFi + PC IP/PORT.
    Serial.println("ReciverConfig::begin() - launching WiFiManager portal to configure WiFi and receiver IP/port");

    // Prepare placeholders with previously-saved values (if any)
    char ip_buffer[40] = {0};
    char port_buffer[16] = {0};
    GSVIpPort(ip_buffer, port_buffer, false);

    // optionally: give small delay to ensure WiFi connected and preferences flushed
    vTaskDelay(pdMS_TO_TICKS(50));
}

void ReciverConfig::load()
{
    std::lock_guard<std::mutex> lock(mu_);
    prefs_.begin(prefs_namespace_,true);
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

//have to fix 
void ReciverConfig::save(const char* ip_str, uint16_t port) 
{
    if (ip_str != nullptr && port != NULL)
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(prefs_namespace_,false);
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
    else if (port == NULL)
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(prefs_namespace_,false);
        prefs_.putString("pc_ip", String(ip_str));
        prefs_.end();
        IPAddress tmp;
        if (ip_str && ip_str[0] && tmp.fromString(String(ip_str)))
        {
            ip_ = tmp;
        }
    }
    else if (ip_str == nullptr)
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(prefs_namespace_,false);
        prefs_.putString("pc_port", String((unsigned)port));
        prefs_.end();
        port_ = port;
    }
}

void ReciverConfig::save(const char* ip_str, uint16_t port)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (prefs_.begin(prefs_namespace_, false))
    {
        Serial.println("ReciverConfig::save::prefs.begin() failed!");
        return;
    }
    

    if (ip_str && ip_str[0])
    {
        IPAddress tmp;
        if (tmp.fromString(String(ip_str)))
        {
            ip_ = tmp;
        }
        else
        {
            ip_ = IPAddress(0,0,0,0);
            prefs_.remove(PREFS_IP_ID);
        }
    }
    if (ip_ != IPAddress(0,0,0,0))
    {
        prefs_.putString(PREFS_IP_ID, String(ip_));
    }
    if (port > 0 && port <= 65535)
    {
        port_ = port;
        prefs_.putUShort(PREFS_PORT_ID, port_);
    }
    
   prefs_.end(); 
    
}

void ReciverConfig::clear()
{
    std::lock_guard<std::mutex> lock(mu_);
    prefs_.begin(prefs_namespace_,false);
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

bool ReciverConfig::ConnectTOReciverIP(WiFiClient* WiFi_TCPClient)
{
    if (WiFi_TCPClient == nullptr)
    {
        return false;
    }
    IPAddress ip_copy;
    uint16_t port_copy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ip_copy = ip_;
        port_copy = port_;
    }
    if (ip_copy == IPAddress(0,0,0,0) || port_copy == 0)
    {
        return false;
    }
    WiFi_TCPClient->stop();
    delay(10);

    bool connection_ok = false;
    if (WiFi_TCPClient->connect(ip_copy, port_copy))
    {
        connection_ok = true;
    }
    if (!connection_ok || !WiFi_TCPClient->connected())
    {
        WiFi_TCPClient->stop();
        Serial.println("ReciverConfig::ConnectTOReciverIP::Connection failed");
        return false;
    }
    bool still_same = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        still_same = (ip_ == ip_copy && port_ == port_copy);
    }
    if (!still_same)
    {
        WiFi_TCPClient->stop();
        Serial.println("ReciverConfig::ConnectTOReciverIP::Reciver configuration changed ip or port::Closing");
        return false;
    }
    
    WiFi_TCPClient->setNoDelay(true);

    Serial.print("ReciverConfig::ConnectTOReciverIP::Reciver IP: ");
    Serial.print(ip_copy.toString());
    Serial.print(":");
    Serial.println(port_copy);

    return true; 
}


bool ReciverConfig::TCPWriteAll(
    WiFiClient* WiFiclient,
    const uint8_t* data,
    size_t len,
    uint32_t timeout_ms,
    int max_retries,
    size_t chunk_size
)
{
    if (!WiFiclient || !data) return false;
    if (len == 0) return true;
    if (chunk_size == 0) chunk_size = 1024;

    size_t total_sent = 0;
    int recon_attempts = 0;

    // Ensure connected (try to connect once)
    if (!WiFiclient->connected()) {
        if (!ConnectTOReciverIP(WiFiclient)) return false;
    }

    // Main send loop
    while (total_sent < len) {
        size_t remaining = len - total_sent;
        size_t to_send = std::min(remaining, chunk_size);

        // per-chunk timeout using wrap-safe pattern
        const uint32_t start_ms = millis();
        bool chunk_sent = false;

        while ((uint32_t)(millis() - start_ms) < timeout_ms) {
            if (!WiFiclient->connected()) break; // try reconnect outside inner loop

            int written = WiFiclient->write(data + total_sent, to_send);
            if (written > 0) {
                total_sent += static_cast<size_t>(written);
                chunk_sent = true;

                // partial write: keep trying for remainder of this chunk
                if (static_cast<size_t>(written) < to_send) {
                    to_send -= static_cast<size_t>(written);
                    // small cooperative pause
                    taskYIELD();
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                } else {
                    // entire chunk sent
                    break;
                }
            }

            // written == 0: socket buffer full or non-blocking; yield and retry
            taskYIELD();
            vTaskDelay(pdMS_TO_TICKS(1));
        } // end per-chunk timeout loop

        if (!chunk_sent) {
            // Failure for this chunk: try reconnecting (with backoff)
            recon_attempts++;
            WiFiclient->stop();
            vTaskDelay(pdMS_TO_TICKS(10 * recon_attempts));

            if (recon_attempts > max_retries) {
                return false;
            }
            if (!ConnectTOReciverIP(WiFiclient)) {
                // failed to reconnect -> small backoff & retry outer loop
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            // reconnected, reset attempt counter and retry sending the same chunk
            recon_attempts = 0;
            continue;
        }

        // successful chunk -> cooperation point
        taskYIELD();
    } // end while total_sent < len

    // Make a best-effort flush (may block until internal buffers are emptied)
    WiFiclient->flush();

    return true;
}
