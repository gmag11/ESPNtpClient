#include <Arduino.h>

//#include "WifiConfig.h"

#include <ESPNtpClient.h>
#ifdef ESP32
#include <WiFi.h>
#else
#include <ESP8266WiFi.h>
#endif

#ifndef WIFI_CONFIG_H
#define YOUR_WIFI_SSID "YOUR_WIFI_SSID"
#define YOUR_WIFI_PASSWD "YOUR_WIFI_PASSWD"
#endif // !WIFI_CONFIG_H

const PROGMEM char* ntpServer = "pool.ntp.org";

#ifndef LED_BUILTIN
#define LED_BUILTIN 5
#endif

boolean syncEventTriggered = false; // True if a time even has been triggered
NTPEvent_t ntpEvent; // Last triggered event

void processSyncEvent (NTPEvent_t ntpEvent) {
    //Serial.printf ("[NTP-event] %d\n", ntpEvent.event);
    if (ntpEvent.event == timeSyncd) {
        Serial.printf ("[NTP-event] Got NTP time: %s from %s:%u. Offset: %0.3f ms. Delay: %0.3f ms\n",
                       NTP.getTimeDateStringUs (),
                       ntpEvent.info.serverAddress.toString ().c_str (),
                       ntpEvent.info.port,
                       ntpEvent.info.offset * 1000,
                       ntpEvent.info.delay * 1000);
    }
}


void setup () {
    Serial.begin (115200);
    Serial.println ();
    WiFi.begin (YOUR_WIFI_SSID, YOUR_WIFI_PASSWD);
    NTP.setTimeZone (TZ_Etc_UTC);
    NTP.onNTPSyncEvent ([] (NTPEvent_t event) {
        ntpEvent = event;
        syncEventTriggered = true;
    });
    NTP.setInterval (300);
    NTP.begin (ntpServer);
    pinMode (LED_BUILTIN, OUTPUT);
    digitalWrite (LED_BUILTIN, HIGH);
}

void loop () {
    timeval currentTime;
    gettimeofday (&currentTime, NULL);
    int64_t us = NTP.micros () % 1000000L;
    digitalWrite (LED_BUILTIN, !(us >= 0 && us < 10000));
    if (syncEventTriggered) {
        syncEventTriggered = false;
        processSyncEvent (ntpEvent);
    }
}
