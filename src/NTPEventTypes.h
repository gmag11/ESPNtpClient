#ifndef _NtpEventTypes_h
#define _NtpEventTypes_h

#ifdef ESP32
#include <WiFi.h>
#else
#include <ESP8266WiFi.h>
#endif

typedef enum {
    timeSyncd = 0, // Time successfully got from NTP server
    noResponse = -1, // No response from server
    invalidAddress = -2, // Address not reachable
    invalidPort = -3, // Port already used
    requestSent = 1, // NTP request sent, waiting for response
    partlySync = 2, // Successful sync but offset was over threshold
    syncNotNeeded = 3, // Successful sync but offset was under minimum threshold
    errorSending = -4, // An error happened while sending the request
    responseError = -5, // Wrong response received
    syncError = -6 // Error adjusting time
} NTPSyncEventType_t;

typedef struct {
    double offset = 0.0;
    double delay = 0.0;
    IPAddress serverAddress;
    unsigned int port = 0;
} NTPSyncEventInfo_t;

typedef struct {
    NTPSyncEventType_t event;
    NTPSyncEventInfo_t info;
} NTPEvent_t;

#endif // _NtpEventTypes_h