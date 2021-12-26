#include <Arduino.h>

//#include "WifiConfig.h"
#define USE_ETHERNET      false             /* false: WiFi  or  true: lwIP_Etherner */
#define USE_STATIC        false             /* false: DHCP  or  true: STATIC */

#include <ESPNtpClient.h>

#ifdef ESP32
  #include <WiFi.h>

  #undef USE_ETHERNET
  #define USE_ETHERNET      false
#else
  #include <ESP8266WiFi.h>
  
  #if (USE_ETHERNET)
    #include <SPI.h>
    #include <ENC28J60lwIP.h>
    
    #define ETH_SS_PIN 4
    ENC28J60lwIP eth(ETH_SS_PIN);
  #endif
#endif //ESP32

#ifndef WIFI_CONFIG_H
  #define YOUR_WIFI_SSID "YOUR_WIFI_SSID"
  #define YOUR_WIFI_PASSWD "YOUR_WIFI_PASSWD"
  
  // YOUR_ROUTER_SETTINGS
  #if (USE_STATIC)
    IPAddress              apIP(192, 168,   1,  10);
    IPAddress              gwIP(192, 168,   1,   1);
    IPAddress              snIP(255, 255, 255,   0);
    IPAddress             dnIP1(192, 168,   1,   1);
    IPAddress             dnIP2(  8,   8,   8,   8);
  #endif
#endif // !WIFI_CONFIG_H

#define SHOW_TIME_PERIOD 1000

// weak functions to get connection status, reconnect and IP address of device
#if (USE_ETHERNET)
  bool connectionStatus() {
      return eth.connected ();
  }
  
  bool connectionReconnect() {
      return true;
  }
  
  IPAddress getDeviceIP() {
      return eth.localIP ();
  }
#endif //USE_ETHERNET

void setup() {
    Serial.begin (115200);
    Serial.println ();
    
    #if (USE_ETHERNET)
        SPI.begin();
        
        eth.setDefault();     // use ethernet for default route
        
        #if (USE_STATIC)
          eth.config(apIP, gwIP, snIP, dnIP1, dnIP2);
        #endif
        
        eth.begin();
        
    #else
        WiFi.mode (WIFI_STA);
        
        #if (USE_STATIC)
          WiFi.config(apIP, gwIP, snIP, dnIP1, dnIP2);
        #endif
        
        WiFi.begin (YOUR_WIFI_SSID, YOUR_WIFI_PASSWD);
        
    #endif //USE_ETHERNET
    
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
