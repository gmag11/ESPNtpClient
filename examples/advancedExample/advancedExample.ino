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

#ifdef ESP32
  #define ONBOARDLED 5 // Built in LED on some ESP-32 boards
#else
  #define ONBOARDLED 2 // Built in LED on ESP-12/ESP-07
#endif

#define SHOW_TIME_PERIOD 1000
#define NTP_TIMEOUT 5000

const PROGMEM char* ntpServer = "pool.ntp.org";
bool conFirstConnected = false;
bool conLastStatus = false;

boolean syncEventTriggered = false; // True if a time even has been triggered
NTPEvent_t ntpEvent; // Last triggered event
double offset;
double timedelay;

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

#if (!USE_ETHERNET)
  #ifdef ESP32
  void onWifiEvent (system_event_id_t event, system_event_info_t info) {
  #else
  void onWifiEvent (WiFiEvent_t event) {
  #endif
      Serial.printf ("[WiFi-event] event: %d\n", event);
      
      switch (event) {
  #ifdef ESP32
      case SYSTEM_EVENT_STA_CONNECTED:
          Serial.printf ("Connected to %s. Asking for IP address.\r\n", info.connected.ssid);
          break;
      case SYSTEM_EVENT_STA_GOT_IP:
          Serial.printf ("Got IP: %s\r\n", IPAddress (info.got_ip.ip_info.ip.addr).toString ().c_str ());
          Serial.printf ("Connected: %s\r\n", WiFi.status () == WL_CONNECTED ? "yes" : "no");
          digitalWrite (ONBOARDLED, LOW); // Turn on LED
          conFirstConnected = true;
          break;
      case SYSTEM_EVENT_STA_DISCONNECTED:
          Serial.printf ("Disconnected from SSID: %s\n", info.disconnected.ssid);
          Serial.printf ("Reason: %d\n", info.disconnected.reason);
          digitalWrite (ONBOARDLED, HIGH); // Turn off LED
          //NTP.stop(); // NTP sync can be disabled to avoid sync errors
          WiFi.reconnect ();
          break;
  #else
      case WIFI_EVENT_STAMODE_CONNECTED:
          Serial.printf ("Connected to %s. Asking for IP address.\r\n", WiFi.BSSIDstr().c_str());
          break;
      case WIFI_EVENT_STAMODE_GOT_IP:
          Serial.printf ("Got IP: %s\r\n", WiFi.localIP().toString().c_str ());
          Serial.printf ("Connected: %s\r\n", WiFi.status () == WL_CONNECTED ? "yes" : "no");
          digitalWrite (ONBOARDLED, LOW); // Turn on LED
          conFirstConnected = true;
          break;
      case WIFI_EVENT_STAMODE_DISCONNECTED:
          Serial.printf ("Disconnected from SSID: %s\n", WiFi.BSSIDstr ().c_str ());
          //Serial.printf ("Reason: %d\n", info.disconnected.reason);
          digitalWrite (ONBOARDLED, HIGH); // Turn off LED
          //NTP.stop(); // NTP sync can be disabled to avoid sync errors
          WiFi.reconnect ();
          break;
  #endif
      default:
          break;
      }
  }

#endif //!USE_ETHERNET

void processSyncEvent (NTPEvent_t ntpEvent) {
    switch (ntpEvent.event) {
        case timeSyncd:
        case partlySync:
        case syncNotNeeded:
        case accuracyError:
            Serial.printf ("[NTP-event] %s\n", NTP.ntpEvent2str (ntpEvent));
            break;
        default:
            break;
    }
}

void setup() {
    Serial.begin (115200);
    Serial.println ("\r\n");
    
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
    
    pinMode (ONBOARDLED, OUTPUT); // Onboard LED
    digitalWrite (ONBOARDLED, HIGH); // Switch off LED
    
    NTP.onNTPSyncEvent ([] (NTPEvent_t event) {
        ntpEvent = event;
        syncEventTriggered = true;
    });
    
    #if (USE_ETHERNET)
      conLastStatus = !eth.connected ();
      
    #else
      WiFi.onEvent (onWifiEvent);
    #endif
}

void loop() {
    static int i = 0;
    static int last = 0;

    if (conFirstConnected) {
        conFirstConnected = false;
        NTP.setTimeZone (TZ_Europe_Madrid);
        NTP.setInterval (600);
        NTP.setNTPTimeout (NTP_TIMEOUT);
        // NTP.setMinSyncAccuracy (5000);
        // NTP.settimeSyncThreshold (3000);
        NTP.begin (ntpServer);
    }

    if (syncEventTriggered) {
        syncEventTriggered = false;
        processSyncEvent (ntpEvent);
    }

    if ((millis () - last) >= SHOW_TIME_PERIOD) {
        last = millis ();
        Serial.print (i); Serial.print (" ");
        Serial.print (NTP.getTimeDateStringUs ()); Serial.print (" ");
        
        #if (USE_ETHERNET)
          if(conLastStatus != eth.connected ())
          {
            conLastStatus = eth.connected ();
          	
            if(conLastStatus)
            {
              Serial.printf ("\r\nGot IP: %s\r\n", eth.localIP().toString().c_str ());
              
              #if (USE_STATIC)
                Serial.println ("Ethernet is connected [STATIC]");
              #else
                Serial.println ("Ethernet is connected [DHCP]");
              #endif
              
              digitalWrite (ONBOARDLED, LOW); // Turn on LED
              conFirstConnected = true;
            }
            else 
            {
              #if (USE_STATIC)
                Serial.println ("Ethernet is not connected [STATIC]");
              #else
                Serial.println ("Ethernet is not connected [DHCP]");
              #endif
              
              digitalWrite (ONBOARDLED, HIGH); // Turn off LED
            }
          }
          
        #else
          Serial.print ("WiFi is ");
          Serial.print (WiFi.isConnected () ? "connected " : "not connected ");
          
          #if (USE_STATIC)
            Serial.println ("[STATIC]");
          #else
            Serial.println ("[DHCP]");
          #endif
          
        #endif //USE_ETHERNET
        
        Serial.print ("Uptime: ");
        Serial.print (NTP.getUptimeString ()); Serial.print (" since ");
        Serial.println (NTP.getTimeDateString (NTP.getFirstSyncUs ()));
        Serial.printf ("Free heap: %u\n", ESP.getFreeHeap ());
        i++;
    }
    delay (0);
}
