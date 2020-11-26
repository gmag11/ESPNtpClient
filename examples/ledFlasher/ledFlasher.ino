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

#define SHOW_TIME_PERIOD 1000

const PROGMEM char* ntpServer = "pool.ntp.org";

#ifndef LED_BUILTIN
#define LED_BUILTIN 5
#endif

boolean syncEventTriggered = false; // True if a time even has been triggered
NTPEvent_t ntpEvent; // Last triggered event

void processSyncEvent (NTPEvent_t ntpEvent) {
    if (!ntpEvent.event) {
        Serial.printf ("[NTP-event] Got NTP time: %s from %s:%u. Offset: %0.3f ms. Delay: %0.3f ms\n",
                       NTP.getTimeDateString (NTP.getLastNTPSyncUs ()),
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
    NTP.begin (ntpServer);
    pinMode (LED_BUILTIN, OUTPUT);
    digitalWrite (LED_BUILTIN, HIGH);
    NTP.onNTPSyncEvent ([] (NTPEvent_t event) {
        ntpEvent = event;
        syncEventTriggered = true;
    });
}

void loop () {
    timeval currentTime;
    gettimeofday (&currentTime, NULL);
    digitalWrite (LED_BUILTIN, !(currentTime.tv_usec >= 0 && currentTime.tv_usec < 50000));
    if (syncEventTriggered) {
        syncEventTriggered = false;
        processSyncEvent (ntpEvent);
    }

}