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
    if (ip_copy == IPAddress(0,0,0,0) || port == 0)
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

bool ReciverConfig::tcpWriteAll(Client* client,
                                const uint8_t* data,
                                size_t len,
                                uint32_t timeout_ms,
                                int max_retries,
                                size_t chunk_size)
{
    if (client == nullptr || data == nullptr) return false;
    if (len == 0) return true;

    // Defensive: prefer a sane chunk size (TCP MSS-ish). Keep at least 1.
    if (chunk_size == 0) chunk_size = 1024;

    size_t total_sent = 0;
    int reconnect_attempts = 0;

    // Local helper to attempt reconnect using ReciverConfig's logic
    auto try_reconnect = [&](Client* c)->bool {
        // Only works for WiFiClient; safe to cast if you store WiFiClient*
        WiFiClient* wclient = dynamic_cast<WiFiClient*>(c);
        if (wclient == nullptr) {
            // We don't have a WiFiClient to reconnect with; caller must manage connection.
            return false;
        }
        // Close, small delay, then try to connect using ReciverConfig helper:
        wclient->stop();
        delay(10);
        return ConnectTOReciverIP(wclient);
    };

    // Main loop: write until everything is sent or we give up
    while (total_sent < len) {
        // Ensure connected; if not, try reconnect up to max_retries
        if (!client->connected()) {
            if (reconnect_attempts >= max_retries) {
                // give up
                client->stop();
                return false;
            }
            if (!try_reconnect(client)) {
                // reconnect failed; backoff and retry
                reconnect_attempts++;
                vTaskDelay(pdMS_TO_TICKS(100 * reconnect_attempts)); // progressive backoff
                continue;
            }
            // successful reconnect — reset attempt counter
            reconnect_attempts = 0;
        }

        // prepare a chunk to send
        size_t remaining = len - total_sent;
        size_t to_send = std::min(remaining, chunk_size);

        // per-chunk timeout window
        const uint32_t chunk_deadline = millis() + timeout_ms;
        bool chunk_sent = false;

        // Try writing until we get bytes accepted or timeout
        while (millis() < chunk_deadline) {
            // attempt write (Client::write returns size_t usually)
            int written = client->write(data + total_sent, to_send);
            if (written > 0) {
                total_sent += (size_t)written;
                chunk_sent = true;
                // If partial write occurred (written < to_send), continue the inner loop to send the remainder
                if ((size_t)written < to_send) {
                    // adjust to_send for the remainder of this chunk
                    to_send = to_send - (size_t)written;
                    // small pause to let TCP stack flush its buffer
                    taskYIELD();
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                } else {
                    // whole chunk accepted; break out to send next chunk
                    break;
                }
            }

            // written == 0: no buffer space right now (or non-blocking) — wait a little
            // If socket became disconnected during this time, break to outer reconnect logic
            if (!client->connected()) break;

            // Let other tasks run and avoid busy looping
            taskYIELD();
            vTaskDelay(pdMS_TO_TICKS(1));
        } // end per-chunk attempt loop

        if (!chunk_sent) {
            // chunk write timed out or socket disconnected.
            // Try reconnect once and then retry; count as a reconnect attempt.
            reconnect_attempts++;
            client->stop();
            vTaskDelay(pdMS_TO_TICKS(10 * reconnect_attempts));
            if (reconnect_attempts > max_retries) {
                return false;
            }
            if (!try_reconnect(client)) {
                // reconnect failed; loop will backoff above
                vTaskDelay(pdMS_TO_TICKS(50));
            } else {
                // reconnected; continue outer loop to retry unsent payload
                continue;
            }
        }

        // small cooperative yield so other tasks (e.g., writer) can run
        taskYIELD();
    } // end while total_sent < len

    // Ensure data is flushed to the network stack if available
    // Note: WiFiClient::flush on ESP may block until tx buffer is empty.
    client->flush();

    return true;
}
