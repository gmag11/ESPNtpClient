#include <Arduino.h>

#include "WifiConfig.h"
#include <ESPNtpClient.h>

#include <WiFi.h>

#ifndef WIFI_CONFIG_H
#define YOUR_WIFI_SSID "YOUR_WIFI_SSID"
#define YOUR_WIFI_PASSWD "YOUR_WIFI_PASSWD"
#endif // !WIFI_CONFIG_H

#define ONBOARDLED 5 // Built in LED on ESP-12/ESP-07
#define SHOW_TIME_PERIOD 1000
#define NTP_TIMEOUT 1500

const PROGMEM char* ntpServer = "es.pool.ntp.org";
bool wifiFirstConnected = false;

boolean syncEventTriggered = false; // True if a time even has been triggered
NTPSyncEvent_t ntpEvent; // Last triggered event
double offset;
double timedelay;

void onWifiEvent (system_event_id_t event, system_event_info_t info) {
    Serial.printf ("[WiFi-event] event: %d\n", event);

    switch (event) {
    case SYSTEM_EVENT_STA_CONNECTED:
        Serial.printf ("Connected to %s. Asking for IP address.\r\n", info.connected.ssid);
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        Serial.printf ("Got IP: %s\r\n", IPAddress (info.got_ip.ip_info.ip.addr).toString ().c_str ());
        Serial.printf ("Connected: %s\r\n", WiFi.status () == WL_CONNECTED ? "yes" : "no");
        digitalWrite (ONBOARDLED, LOW); // Turn on LED
        wifiFirstConnected = true;
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        Serial.printf ("Disconnected from SSID: %s\n", info.disconnected.ssid);
        Serial.printf ("Reason: %d\n", info.disconnected.reason);
        digitalWrite (ONBOARDLED, HIGH); // Turn off LED
        //NTP.stop(); // NTP sync can be disabled to avoid sync errors
        WiFi.reconnect ();
        break;
    default:
        break;
    }
}

void processSyncEvent (NTPSyncEvent_t ntpEvent, double offset, double tdelay) {
    if (ntpEvent < 0) {
        Serial.printf ("Time Sync error %d:", ntpEvent);
        if (ntpEvent == noResponse) {
            Serial.println ("NTP server not reachable");
        } else if (ntpEvent == invalidAddress) {
            Serial.println ("Invalid NTP server address");
        }
    } else if (!ntpEvent) {
        Serial.printf ("Got NTP time: %s Offset: %0.3f ms Delay: %0.3f ms\n",
                       NTP.getTimeDateString (NTP.getLastNTPSyncUs ()),
                       offset, timedelay);
    } else {
        Serial.println ("NTP request Sent");
    }
}


void setup() {
    Serial.begin (115200);
    Serial.println ();
    WiFi.mode (WIFI_STA);
    WiFi.begin (YOUR_WIFI_SSID, YOUR_WIFI_PASSWD);
    
    pinMode (ONBOARDLED, OUTPUT); // Onboard LED
    digitalWrite (ONBOARDLED, HIGH); // Switch off LED
    
    NTP.onNTPSyncEvent ([] (NTPSyncEvent_t event, double toffset, double tdelay) {
        ntpEvent = event;
        syncEventTriggered = true;
        offset = toffset * 1000;
        timedelay = tdelay * 1000;
    });
    WiFi.onEvent (onWifiEvent);
}

void loop() {
    static int i = 0;
    static int last = 0;

    if (wifiFirstConnected) {
        wifiFirstConnected = false;
        NTP.setTimeZone (TZ_Etc_UTC);
        NTP.setInterval (30);
        NTP.setNTPTimeout (NTP_TIMEOUT);
        NTP.begin (ntpServer);
    }

    if (syncEventTriggered) {
        processSyncEvent (ntpEvent, offset, timedelay);
        syncEventTriggered = false;
    }

    if ((millis () - last) > SHOW_TIME_PERIOD) {
        last = millis ();
        Serial.print (i); Serial.print (" ");
        Serial.print (NTP.getTimeDateStringUs ()); Serial.print (". ");
        Serial.print ("WiFi is ");
        Serial.print (WiFi.isConnected () ? "connected" : "not connected"); Serial.print (". ");
        Serial.print ("Uptime: ");
        Serial.print (NTP.getUptimeString ()); Serial.print (" since ");
        Serial.println (NTP.getTimeDateString (NTP.getFirstSyncUs ()));
        Serial.printf ("Free heap: %u\n", ESP.getFreeHeap ());
        i++;
    }
    delay (0);
}