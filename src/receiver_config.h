#ifndef RECEIVER_CONFIG_H
#define RECEIVER_CONFIG_H

#include <Preferences.h>
#include <mutex>
#include <IPAddress.h>
#include <Arduino.h>

// NOTE: keep PREF_NAMESPACE visible to the implementation
static constexpr char PREF_NAMESPACE[] = "config";

class ReciverConfig {
public:
  ReciverConfig();
  ~ReciverConfig();

  void begin();
  void load();
  void save(const char* ip_str, uint16_t port);
  void clear();
  void get(IPAddress &out_ip, uint16_t &out_port);
  String ipString() const;
  unsigned short port() const;
  bool isvalid() const;

private:
  // implementation details
  Preferences prefs_;
  mutable std::mutex mu_;   // mutable so const methods can lock
  IPAddress ip_;
  uint16_t port_;
};


#endif // RECEIVER_CONFIG_H
