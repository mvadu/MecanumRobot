#include <Arduino.h>

enum WifiStatus
{
    Undefined = 0,
    NotConnected = 1,
    Connecting = 2,
    Connected = 3,
    ConnectFailed = 4
};

class Serializable
{
public:
    virtual size_t size() const = 0;
    virtual bool serialize(char *dataOut) const = 0;
    virtual bool deserialize(const char *dataIn) = 0;
};

class ConfigManager
{
public:
    static bool SaveConfig(const Serializable &config, String name);
    static bool LoadConfig(String name, Serializable &config);
};

class WiFiConfig : public Serializable
{
public:
    String ssid;
    String password;
    String clientName;
    bool valid;
    size_t size() const override;
    bool serialize(char *dataOut) const override;
    bool deserialize(const char *dataIn) override;
};
