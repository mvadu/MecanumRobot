#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <esp_system.h>
#include <esp_spi_flash.h>
#include <rom/rtc.h>
#include <Update.h>
#include <StreamString.h>
#include "soc/efuse_reg.h"
#include <driver/gpio.h>
#include <Wire.h>

#include "config.hpp"
#include "web_pages.h"
#include "ChipInfo.hpp"
#include <commondata.h>

void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
wl_status_t initWifiConnection();
void fadeLedCallback(TimerHandle_t xTimer);
void connectionFailed();
void wifiConnected();
void IRAM_ATTR resetModule();
void SetupAccessPoint();
bool startMainserver();
void getChipInfo(AsyncWebServerRequest *request);
String downcaseAndRemoveLocalPrefix(const String hostName);
void onNotFound(AsyncWebServerRequest *request);
void serveStatic(AsyncWebServerRequest *request, const uint8_t *page, size_t size);
void getWifiStatus(AsyncWebServerRequest *request);
int ScanWiFi(AsyncResponseStream *response);
void setConfig(AsyncWebServerRequest *request);
void getConfig(AsyncWebServerRequest *request);
void onLoggerEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void onRemoteEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len);
bool CheckMotorControllerConnection();
bool SetMotorDirection();

enum class RemoteMessage : uint8_t
{
    Remote_Dir = 1,
    Remote_Speed = 2,
    Request_Status = 0xF0,
    Heartbeat_Req = 0xAA,
    Heartbeat_Resp = 0xBB,
    Close_Connection = 0xCC,
    Confirm_Connection = 0xDD,
    Motor_Lost = 0xE0,
    Motor_Restored = 0xE1
};

static_assert(sizeof(RemoteMessage) == 1, "!");

class HeartbeatMessage
{
public:
    RemoteMessage code;
    uint8_t reqCount;
};

String hostName;
unsigned long id;
ChipInfo *chipInfo;
WifiStatus wifiStatus = WifiStatus::Undefined;
IPAddress gateway;
volatile RTC_NOINIT_ATTR bool resetConfig;
WiFiConfig wifiConfig;

/////// Handle long press of the button to reset WiFi credentials
const uint8_t resetBtn = 0; //GPIO0, do.it board has a button attached
volatile long btnPressed;
const unsigned long interval = 30;
uint8_t heartbeatCount;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
TimerHandle_t timerFadeLed;

unsigned long lastReceived = 0, loopCount = 0, heartbeatTs = 0;
DirMessage msg(Direction::Stop);
Direction newDir;
bool lostMotorConnection;

AsyncWebServer server(80);
AsyncWebSocket loggerSocket("/logs");
AsyncWebSocket remoteControlSocket("/remote");
AsyncWebSocketClient *remoteContol = NULL;
AsyncWebSocketClient *logger = NULL;

const uint8_t leonardo_Sda = 13;
const uint8_t leonardo_Scl = 14;

void setup()
{
    Serial.begin(115200);

    loggerSocket.onEvent(onLoggerEvent);
    remoteControlSocket.onEvent(onRemoteEvent);
    server.addHandler(&loggerSocket);
    server.addHandler(&remoteControlSocket);

    id = (ESP.getEfuseMac() >> 16) & 0xFFFFFFFF;
    hostName = "esp_";
    hostName.concat(id);

    delay(2000);
    chipInfo = new ChipInfo();
    if (chipInfo->reason == POWERON_RESET || chipInfo->reason == RTCWDT_RTC_RESET)
        resetConfig = false;

    if (!resetConfig)
    {
        ConfigManager::LoadConfig("WiFiConfig", wifiConfig);
        Serial.printf("Found wifiConfig %s, %s, %s, %d \n", wifiConfig.ssid.c_str(), wifiConfig.password.c_str(), wifiConfig.clientName.c_str(), wifiConfig.valid);
    }

    pinMode(LED_BUILTIN, OUTPUT); // set the LED pin mode
    ledcAttachPin(LED_BUILTIN, 1);
    ledcSetup(1, 12000, 16);

    timerFadeLed = xTimerCreate(
        "ledFadeTimer",
        pdMS_TO_TICKS(500),
        pdTRUE,
        (void *)0,
        fadeLedCallback);

    hw_timer_t *timer = timerBegin(0, 80, true);     //timer 0, div 80 with 8MHz, its 1us
    timerAttachInterrupt(timer, &resetModule, true); //attach callback
    timerAlarmWrite(timer, 100000, true);            //  1sec
    timerAlarmEnable(timer);                         //enable interrupt
    if (!wifiConfig.valid)
    {
        Serial.println("No valid WiFi credentials found, starting WiFi wifiConfig wizard");
        SetupAccessPoint();
    }
    else
    {

        hostName = wifiConfig.clientName;
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        WiFi.setHostname(hostName.c_str());
        initWifiConnection();
    }
    delay(5000);
    Wire.begin(leonardo_Sda, leonardo_Scl, 100000L);
    Wire.setTimeOut(100);
    Serial.printf("I2C frequency : %zu\n", Wire.getClock());

    lostMotorConnection = !CheckMotorControllerConnection();
}

void loop()
{
    yield();
    if (millis() - lastReceived > 2000 && remoteContol != NULL)
    {
        RemoteMessage rmsg = RemoteMessage::Request_Status;
        remoteContol->binary((uint8_t *)&rmsg, sizeof(rmsg));
    }

    if (remoteContol != NULL && heartbeatCount > 0 && (millis() - heartbeatTs) > 5000)
    {
        Serial.printf("Heartbeat not received in 5 sec timeout %u %u", remoteContol->id(), heartbeatCount);
        remoteContol->close();
        remoteContol = NULL;
        heartbeatCount = 0;
    }
    loopCount++;
    if (newDir != msg.dir)
    {
        if (logger != NULL)
            logger->printf("Dir changed : %d -> %d", msg.dir, newDir);
        msg.dir = newDir;
        if (!SetMotorDirection() and logger != NULL)
            logger->printf("Unable to set motor direction");
    }

    if (lostMotorConnection && CheckMotorControllerConnection())
    {
        lostMotorConnection = false;
        if (remoteContol != NULL)
        {
            RemoteMessage rmsg = RemoteMessage::Motor_Restored;
            remoteContol->binary((uint8_t *)&rmsg, sizeof(rmsg));
        }
    }
    else if (loopCount % 10 == 0 and !CheckMotorControllerConnection())
    {
        lostMotorConnection = true;
        if (remoteContol != NULL)
        {
            RemoteMessage rmsg = RemoteMessage::Motor_Lost;
            remoteContol->binary((uint8_t *)&rmsg, sizeof(rmsg));
        }
    }

    if (loopCount > 1000)
    {
        loggerSocket.cleanupClients(1);
        loopCount = 0;
    }
    delay(500);
}

void SetupAccessPoint()
{
    DNSServer dnsServer;
    const char *password = "espressif";
    bool exitConfigWizard = false;

    server.onNotFound([](AsyncWebServerRequest *request) {
        onNotFound(request);
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
              //Serial.printf("client host: %s\n", request->host().c_str());
              if (downcaseAndRemoveLocalPrefix(request->host()) != downcaseAndRemoveLocalPrefix(hostName))
              {
                  onNotFound(request);
              }
              serveStatic(request, WiFisetup_html, sizeof(WiFisetup_html));
          })
        .setFilter(ON_AP_FILTER);

    server.on("/getChipInfo", HTTP_GET, [](AsyncWebServerRequest *request) {
              getChipInfo(request);
          })
        .setFilter(ON_AP_FILTER);

    server.on("/getApList", HTTP_GET, [](AsyncWebServerRequest *request) {
              AsyncResponseStream *response = request->beginResponseStream("application/json");
              response->addHeader("Cache-Control", "max-age=-1");
              ScanWiFi(response);
              request->send(response);
          })
        .setFilter(ON_AP_FILTER);

    server.on("/connect", HTTP_POST, [](AsyncWebServerRequest *request) {
              AsyncResponseStream *response = request->beginResponseStream("text/plain");
              response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
              response->addHeader("Expires", "-1");
              if (request->hasParam("client", true))
              {
                  wifiConfig.clientName = request->getParam("client", true)->value();
                  if (wifiConfig.clientName == NULL or wifiConfig.clientName == "")
                  {
                      wifiConfig.clientName = hostName;
                  }
              }
              if ((wifiStatus == WifiStatus::Undefined || wifiStatus == WifiStatus::ConnectFailed) &&
                  request->hasParam("ssid", true) && request->hasParam("password", true))
              {
                  wifiConfig.ssid = request->getParam("ssid", true)->value();
                  wifiConfig.password = request->getParam("password", true)->value();
                  wifiStatus = WifiStatus::NotConnected;
              }
              response->printf("%d", wifiStatus);
              request->send(response);
          })
        .setFilter(ON_AP_FILTER);

    server.on("/getWifiStatus", HTTP_GET, [](AsyncWebServerRequest *request) {
              getWifiStatus(request);
          })
        .setFilter(ON_AP_FILTER);

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
              if (request->hasParam("client", true))
              {
                  wifiConfig.clientName = request->getParam("client", true)->value();
                  if (wifiConfig.clientName == NULL or wifiConfig.clientName == "")
                  {
                      wifiConfig.clientName = hostName;
                  }
              }
              Serial.printf("Saving wifiConfig %s, %s, %s, %d \n", wifiConfig.ssid.c_str(), wifiConfig.password.c_str(), wifiConfig.clientName.c_str(), wifiConfig.valid);
              ConfigManager::SaveConfig(wifiConfig, "WiFiConfig");
              request->send(204); //success, No data
          })
        .setFilter(ON_AP_FILTER);

    server.on("/exit", HTTP_GET, [&](AsyncWebServerRequest *request) {
              exitConfigWizard = true;
              request->send(204); //success, No data
          })
        .setFilter(ON_AP_FILTER);

    WiFi.softAP(hostName.c_str(), password);
    Serial.printf("AP Name: %s - Gateway IP: %s\n", hostName.c_str(), WiFi.softAPIP().toString().c_str());
    dnsServer.start(53, "*", WiFi.softAPIP());
    server.begin();

    while (!exitConfigWizard)
    {
        yield();
        dnsServer.processNextRequest();
        if (wifiStatus == WifiStatus::NotConnected)
        {
            initWifiConnection();
        }
    }

    if (wifiStatus == WifiStatus::Connected)
    {
        delay(2000);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        Serial.println("WiFi setup complete, rebooting..");
        delay(1000);
        ESP.restart();
    }
}

bool startMainserver()
{
    if (!WiFi.isConnected())
        return false;
    Serial.println("Starting main http server");
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        serveStatic(request, main_portal_html, sizeof(main_portal_html));
    });

    server.on("/getWifiStatus", HTTP_GET, [](AsyncWebServerRequest *request) {
        getWifiStatus(request);
    });

    server.on("/getChipInfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        getChipInfo(request);
    });

    server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200);
        ESP.restart();
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        onNotFound(request);
    });

    server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        serveStatic(request, config_html, sizeof(config_html));
    });

    server.on("/setConfig", HTTP_POST, [](AsyncWebServerRequest *request) {
        setConfig(request);
    });

    server.on("/getConfig", HTTP_GET, [](AsyncWebServerRequest *request) {
        getConfig(request);
    });

    DefaultHeaders::Instance().addHeader("server", "ESP32");
    server.begin();
    Serial.println("http server started");
    return true;
}

int ScanWiFi(AsyncResponseStream *response)
{
    //scan state in Async mode, default value is -2/scan failed
    int numberOfNetworks = WiFi.scanComplete();
    //start a new scan if previous one failed
    if (numberOfNetworks == WIFI_SCAN_FAILED)
    {
        WiFi.scanNetworks(true);
        Serial.println("Start scanning for networks");
        response->printf("{\"accessPoints\":[]}");
    }
    else if (numberOfNetworks)
    {
        Serial.printf("Number of networks found: %d \n", numberOfNetworks);
        response->printf("{\"accessPoints\":[");
        for (int i = 0; i < numberOfNetworks; i++)
        {
            response->printf("{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1);
            if (i + 1 < numberOfNetworks)
                response->printf(",");
        }
        response->printf("]}");
        //delete last scan result
        WiFi.scanDelete();
        if (numberOfNetworks == WIFI_SCAN_FAILED)
        {
            WiFi.scanNetworks(true);
            Serial.println("Refresh networks");
        }
        //assume previous attempt failed
        wifiStatus = WifiStatus::ConnectFailed;
    }
    return numberOfNetworks;
}

void IRAM_ATTR resetModule()
{
    portENTER_CRITICAL_ISR(&timerMux);
    if (digitalRead(resetBtn) == HIGH)
    {                   // assumes btn is LOW when pressed
        btnPressed = 0; // btn not pressed so reset clock
    }
    else if ((++btnPressed) >= interval)
    {
        resetConfig = true;
        ESP.restart();
    }
    portEXIT_CRITICAL_ISR(&timerMux);
}

void fadeLedCallback(TimerHandle_t xTimer)
{
    static uint16_t dc = 0;
    noInterrupts();
    ledcWrite(1, dc);
    dc += 500;
}

void connectionFailed()
{
    wifiStatus = WifiStatus::ConnectFailed;
    xTimerStop(timerFadeLed, 0);
    ledcWrite(1, 0);
}

void wifiConnected()
{
    wifiStatus = WifiStatus::Connected;
    if (!wifiConfig.valid)
        wifiConfig.valid = true;
    else
    {
        startMainserver();
    }
    xTimerStop(timerFadeLed, 0);
    ledcWrite(1, 16000);
}

void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
    Serial.printf("[WiFi-event] event: %d timestamp:%lu\n", event, millis());
    switch (event)
    {
    case SYSTEM_EVENT_STA_GOT_IP:
        gateway = IPAddress(info.got_ip.ip_info.gw.addr);
        Serial.printf("WiFi connected IP address: %s ; From: %s, duration %lu\n",
                      WiFi.localIP().toString().c_str(), gateway.toString().c_str(), millis());
        wifiConnected();
        break;
    case SYSTEM_EVENT_STA_START:
        //set sta hostname here
        WiFi.setHostname(hostName.c_str());
        break;
    case SYSTEM_EVENT_STA_LOST_IP:
        Serial.printf("WiFi lost IP");
        wifiStatus = WifiStatus::NotConnected;
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
    {
        wifi_err_reason_t reason = (wifi_err_reason_t)info.disconnected.reason;
        switch (reason)
        {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_802_1X_AUTH_FAILED:
            Serial.printf("WiFi Auth failed");
            connectionFailed();
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
}

wl_status_t initWifiConnection()
{
    if (WiFi.status() == WL_CONNECTED)
        return WL_CONNECTED;

    Serial.printf("Trying to connect - %s, %s\n", wifiConfig.ssid.c_str(), wifiConfig.password.c_str());
    xTimerStart(timerFadeLed, 0);
    wifiStatus = WifiStatus::Connecting;
    WiFi.disconnect(true);  // delete old wifiConfig
    WiFi.persistent(false); //Avoid to store Wifi configuration in Flash
    WiFi.onEvent(WiFiEvent);
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    return WiFi.begin(wifiConfig.ssid.c_str(), wifiConfig.password.c_str());
}

String downcaseAndRemoveLocalPrefix(const String hostName)
{
    String host = hostName;
    host.toLowerCase();
    host.replace(".local", "");
    return host;
}

void onNotFound(AsyncWebServerRequest *request)
{
    StreamString s;
    //Serial.printf("Host: %s", request->host().c_str());
    if (downcaseAndRemoveLocalPrefix(request->host()) != hostName)
    {
        s.printf("http://%s.local/", hostName.c_str());
        request->redirect(s);
    }
    else
        request->send(404); //Sends 404 File Not Found
}

void serveStatic(AsyncWebServerRequest *request, const uint8_t *page, size_t size)
{
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", page, size);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "max-age=86400");
    request->send(response);
}

void getWifiStatus(AsyncWebServerRequest *request)
{
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Expires", "-1");
    response->printf("{\"status\":%d", wifiStatus);
    if (wifiStatus == WifiStatus::Connected)
    {
        response->printf(",\"IP\":\"%s\",\"AP\":\"%s\",\"Name\":\"%s\"",
                         WiFi.localIP().toString().c_str(), wifiConfig.ssid.c_str(), wifiConfig.clientName.c_str());
    }
    response->printf("}");
    request->send(response);
}

void getChipInfo(AsyncWebServerRequest *request)
{
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "max-age=86400");
    response->printf("{");
    response->printf("\"reason\":%d,", chipInfo->reason);
    response->printf("\"sdkVersion\":\"%s\",", chipInfo->sdkVersion);
    response->printf("\"chipVersion\":%d,", chipInfo->chipVersion);
    response->printf("\"coreCount\":%d,", chipInfo->coreCount);
    response->printf("\"featureBT\":%d,", chipInfo->featureBT);
    response->printf("\"featureBLE\":%d,", chipInfo->featureBLE);
    response->printf("\"featureWiFi\":%d,", chipInfo->featureWiFi);
    response->printf("\"internalFlash\":%d,", chipInfo->internalFlash);
    response->printf("\"flashSize\":%d,", chipInfo->flashSize);
    request->send(response);
}

void setConfig(AsyncWebServerRequest *request)
{
    AsyncResponseStream *response = request->beginResponseStream("text/plain");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Expires", "-1");
    if (request->hasParam("config", true))
    {
        if (request->getParam("config", true)->value() == "NetParams")
        {
            bool updated = false;
            if (request->hasParam("ssid", true))
            {
                wifiConfig.ssid = request->getParam("ssid", true)->value();
                updated = true;
            }
            if (request->hasParam("password", true))
            {
                wifiConfig.password = request->getParam("password", true)->value();
                updated = true;
            }
            if (request->hasParam("client", true))
            {
                wifiConfig.clientName = request->getParam("client", true)->value();
                updated = true;
            }
            if (updated)
            {
                if (ConfigManager::SaveConfig(wifiConfig, "WiFiConfig"))
                    response->print("OK");
                else
                    response->print("Unable to Save");
            }
            else
            {
                response->print("OK");
            }
        }
        else
        {
            response->printf("Invalid Request, Config Param:%s", request->getParam("config")->value().c_str());
        }
    }
    else
    {
        response->printf("Invalid Request, Config Param missing");
    }

    request->send(response);
}

void getConfig(AsyncWebServerRequest *request)
{
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Expires", "-1");
    if (request->hasParam("config"))
    {
        if (request->getParam("config")->value() == "NetParams")
        {
            response->printf("{\"ssid\":%s,\"password\":%s,\"client\":%s,\"client\":%d}",
                             wifiConfig.ssid.c_str(), "", wifiConfig.clientName.c_str(), wifiConfig.valid ? 1 : 0);
        }
        else
        {
            response->printf("Invalid config:%s", request->getParam("config")->value().c_str());
        }
    }
    else
    {
        response->print("Missing Param:config");
    }
    request->send(response);
}

void onLoggerEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{

    switch (type)
    {
    case WS_EVT_CONNECT:
        Serial.printf("Logger client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
        client->printf("Hello Client %u on %s", client->id(), server->url());
        client->keepAlivePeriod(5);
        if (logger != NULL)
        {
            logger->printf("another logger connected..");
            delay(10);
            logger->close();
        }
        logger = client;
        break;
    case WS_EVT_DISCONNECT:
        Serial.printf("logger[%s][%u] disconnect\n", server->url(), client->id());
        logger = NULL;
        break;
    case WS_EVT_PONG:
        Serial.printf("logger[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char *)data : "");
        break;
    case WS_EVT_ERROR:
        Serial.printf("logger[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t *)arg), (char *)data);
        break;
    case WS_EVT_DATA:
        AwsFrameInfo *info = (AwsFrameInfo *)arg;
        Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);
        break;
    }
}

void onRemoteEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{

    switch (type)
    {
    case WS_EVT_CONNECT:
        Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
        client->printf("Hello Client %u on %s", client->id(), server->url());
        client->keepAlivePeriod(5);
        if (remoteContol != NULL)
        {
            remoteContol->ping();
            heartbeatCount++;
            HeartbeatMessage hmsg{RemoteMessage::Heartbeat_Req, heartbeatCount};
            remoteContol->binary((uint8_t *)&hmsg, sizeof(hmsg));
            heartbeatTs = millis();
            client->printf("Currently another remote is connected.. checking if we can switch connections..");
            Serial.println("ping prior client");
        }
        else
        {
            remoteContol = client;
            RemoteMessage rmsg = RemoteMessage::Confirm_Connection;
            remoteContol->binary((uint8_t *)&rmsg, sizeof(rmsg));
            Serial.println("set remote");
        }

        break;
    case WS_EVT_DISCONNECT:
        Serial.printf("remote[%s][%u] disconnect\n", server->url(), client->id());
        if (remoteContol != NULL && client->id() == remoteContol->id())
        {
            Serial.println("no active remote");
            remoteContol = NULL;
        }
        break;
    case WS_EVT_DATA:
        if (remoteContol != NULL && client->id() == remoteContol->id())
            handleWebSocketMessage(arg, data, len);
        break;
    case WS_EVT_PONG:
        Serial.printf("remote[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char *)data : "");
        break;
    case WS_EVT_ERROR:
        Serial.printf("remote[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t *)arg), (char *)data);
        break;
    }
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len)
{
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_BINARY)
    {
        //First byte in the message defines the type. rest of the data is defined by type.
        uint8_t *ptr = data;
        RemoteMessage rmsg = (RemoteMessage)*ptr;
        switch (rmsg)
        {
        case RemoteMessage::Remote_Dir:
            if (len == 2)
            {
                ptr++;
                uint8_t t = *(ptr);
                lastReceived = millis();
                newDir = (Direction)t;
                Serial.printf("Dir : %u\n", t);
            }
            break;
        case RemoteMessage::Remote_Speed:
            break;
        case RemoteMessage::Heartbeat_Req:
            if (len == sizeof(HeartbeatMessage))
            {
                HeartbeatMessage hmsg;
                memcpy(&hmsg, ptr, len);
                if (hmsg.reqCount > 1)
                    Serial.printf("Client thinks %u messages missed \n", hmsg.reqCount);
                hmsg.code = RemoteMessage::Heartbeat_Resp;
                hmsg.reqCount = heartbeatCount;
                remoteContol->binary((uint8_t *)&hmsg, sizeof(hmsg));
            }
            break;
        case RemoteMessage::Heartbeat_Resp:
            if (len == sizeof(HeartbeatMessage))
            {
                Serial.printf("We have %u clients\n", remoteControlSocket.count());
                heartbeatCount = 0; //no point in waiting for other heartbeats, we have a connection
                if (remoteControlSocket.count() > 1)
                {

                    for (const auto &c : remoteControlSocket.getClients())
                    {
                        Serial.printf("checking %u\n", c->id());
                        if (c->id() != remoteContol->id())
                        {
                            Serial.printf("Disconnect %u \n", c->id());
                            c->text("Current controller is still active.. Disconnecting.");
                            RemoteMessage rmsg = RemoteMessage::Close_Connection;
                            remoteContol->binary((uint8_t *)&rmsg, sizeof(rmsg));
                        }
                    }
                }
            }
            break;
        default:
            Serial.printf("Message Len: %u \n", len);
            break;
        }
    }
}

bool CheckMotorControllerConnection()
{
    Wire.beginTransmission(motorController);
    Wire.write((uint8_t)Command::PresenceCheck);
    if (Wire.endTransmission() != 0)
        return false;
    delay(100);
    Wire.requestFrom(motorController, (uint8_t)1);
    uint8_t ret = Wire.read();
    //Serial.printf("i2c ret: %u", ret);
    return (ret == motorController);
}

bool SetMotorDirection()
{
    Wire.beginTransmission(motorController);
    size_t d = Wire.write((const uint8_t *)&msg, sizeof(msg));
    bool d1 = Wire.endTransmission();
    //Serial.printf("i2c wrote %zu bytes out of %u, end status: %d \n", d, sizeof(msg),d1);
    return d1 == 0;
}