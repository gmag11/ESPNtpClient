#include <Arduino.h>

//#include "WifiConfig.h"

#define USE_ETHERNET true        /* false: WiFi  or  true: lwIP_Etherner */

#include <ESPNtpClient.h>
#ifdef ESP32
#include <WiFi.h>
#else
#include <ESP8266WiFi.h>

#if (USE_ETHERNET)
  #include <SPI.h>
  #include <ENC28J60lwIP.h>
  
  #define ETH_SS_PIN 4
  ENC28J60lwIP eth(ETH_SS_PIN);
#endif
#endif

#ifndef WIFI_CONFIG_H
#define YOUR_WIFI_SSID "YOUR_WIFI_SSID"
#define YOUR_WIFI_PASSWD "YOUR_WIFI_PASSWD"
#endif // !WIFI_CONFIG_H

#define SHOW_TIME_PERIOD 1000

// weak functions to get connection status, reconnect and IP address of device
bool connectionStatus() {
  #if (USE_ETHERNET)
      return eth.connected ();
  #else
      return WiFi.isConnected ();
  #endif
}
bool connectionReconnect() {
  #if (USE_ETHERNET)
      return true;
  #else
      return WiFi.reconnect ();
  #endif
}
IPAddress getDeviceIP() {
  #if (USE_ETHERNET)
      return eth.localIP ();
  #else
      return WiFi.localIP ();
  #endif
}

void setup() {
    Serial.begin (115200);
    Serial.println ();
    
    #if (USE_ETHERNET)
        SPI.begin();
        
        eth.setDefault();     // use ethernet for default route
        eth.begin();
    #else
        WiFi.begin (YOUR_WIFI_SSID, YOUR_WIFI_PASSWD);
    #endif
    
    NTP.setTimeZone (TZ_Etc_UTC);
    NTP.begin ();
}

void loop() {
    static int last = 0;

    if ((millis () - last) >= SHOW_TIME_PERIOD) {
        last = millis ();
        Serial.println (NTP.getTimeDateStringUs ());
    }
}
