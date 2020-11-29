/*
 Name:		ESPNtpClient
 Created:	28/11/2020
 Author:	Germán Martín (github@gmartin.net)
 Maintainer:Germán Martín (github@gmartin.net)

 Library to get system sync from a NTP server with microseconds accuracy
*/

#ifndef _EspNtpClient_h
#define _EspNtpClient_h

#if defined(ARDUINO) && ARDUINO >= 100
#include "Arduino.h"
#else
#include "WProgram.h"
#endif

#include <functional>
using namespace std;
using namespace placeholders;

extern "C" {
#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "lwip/dns.h"
#include "sys/time.h"
#ifdef ESP32
#include "include/time.h"
#else
#include "time.h"
#endif
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
}

#ifdef ESP32
#include "TZdef.h"
#else
#include "TZ.h"
#endif

constexpr auto DEFAULT_NTP_SERVER = "pool.ntp.org"; // Default international NTP server. I recommend you to select a closer server to get better accuracy
constexpr auto DEFAULT_NTP_PORT = 123; // Default local udp port. Select a different one if neccesary (usually not needed)
constexpr auto DEFAULT_NTP_INTERVAL = 1800; // Default sync interval 30 minutes
constexpr auto DEFAULT_NTP_SHORTINTERVAL = 15; // Sync interval when sync has not been achieved. 15 seconds
constexpr auto FAST_NTP_SYNCNTERVAL = 2000; // Sync interval when sync has not reached required accuracy in ms
constexpr auto DEFAULT_NTP_TIMEOUT = 1500; // Default NTP timeout ms
constexpr auto MIN_NTP_TIMEOUT = 100; // Minumum admisible ntp timeout in ms
constexpr auto MIN_NTP_INTERVAL = 5; // Minumum NTP request interval in seconds
constexpr auto DEFAULT_MIN_SYNC_ACCURACY_US = 5000; // Minimum sync accuracy in us
constexpr auto DEFAULT_MAX_RESYNC_RETRY = 4; // Maximum number of sync retrials if offset is above accuravy
#ifdef ESP8266
constexpr auto ESP8266_LOOP_TASK_INTERVAL = 500; // Loop task period on ESP8266
constexpr auto ESP8266_RECEIVER_TASK_INTERVAL = 100; // Receiver task period on ESP8266
#endif // ESP8266
constexpr auto DEFAULT_TIME_SYNC_THRESHOLD = 2500; // If calculated offset is less than this in us clock will not be corrected
constexpr auto DEFAULT_NUM_OFFSET_AVE_ROUNDS = 3; // Number of NTP request and response rounds to calculate offset average

constexpr auto TZNAME_LENGTH = 60; // Max TZ name description length
constexpr auto SERVER_NAME_LENGTH = 40; // Max server name (FQDN) length
constexpr auto NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message

/* Useful Constants */
constexpr auto SECS_PER_MIN = ((time_t)(60UL));
constexpr auto SECS_PER_HOUR = ((time_t)(3600UL));
constexpr auto SECS_PER_DAY = ((time_t)(SECS_PER_HOUR * 24UL));
constexpr auto DAYS_PER_WEEK = ((time_t)(7UL));
constexpr auto SECS_PER_WEEK = ((time_t)(SECS_PER_DAY * DAYS_PER_WEEK));
constexpr auto SECS_PER_YEAR = ((time_t)(SECS_PER_DAY * 365UL));
constexpr auto SECS_YR_2000 = ((time_t)(946684800UL)); // the time at the start of y2k

#ifdef ESP32
#include <WiFi.h>
#else
#include <ESP8266WiFi.h>
#endif
#include <Ticker.h>

#include "NTPEventTypes.h"

typedef enum NTPStatus {
    syncd = 0, // Time synchronized correctly
    unsyncd = -1, // Time may not be valid
    ntpRequested = 1, // NTP request sent, waiting for response
    partialSync = 2 // NPT is synchronised but precission is below threshold
} NTPStatus_t; // Only for internal library use

typedef struct {
    int li;
    int vers;
    int mode;
} NTPFlags_t;

typedef struct {
    bool valid = false;
    NTPFlags_t flags;
    uint8_t peerStratum;
    uint8_t pollingInterval;
    uint8_t clockPrecission;
    uint32_t rootDelay;
    uint32_t dispersion;
    uint8_t refID[4];
    timeval reference;
    timeval origin;
    timeval receive;
    timeval transmit;
} NTPPacket_t;

typedef std::function<void (NTPEvent_t)> onSyncEvent_t;

static char strBuffer[35];

class NTPClient {
protected:
    udp_pcb* udp;                   ///< UDP connection object
    timeval lastSyncd;      ///< Stored time of last successful sync
    timeval firstSync;      ///< Stored time of first successful sync after boot
    timeval packetLastReceived; ///< Moment when a NTP response has arrived
    unsigned long uptime = 0;  ///< Time since boot
    unsigned int shortInterval = DEFAULT_NTP_SHORTINTERVAL * 1000;    ///< Interval to set periodic time sync until first synchronization.
    unsigned int longInterval = DEFAULT_NTP_INTERVAL * 1000;          ///< Interval to set periodic time sync
    unsigned int actualInterval = DEFAULT_NTP_SHORTINTERVAL * 1000;   ///< Currently selected interval
    onSyncEvent_t onSyncEvent;  ///< Event handler callback
    uint16_t ntpTimeout = DEFAULT_NTP_TIMEOUT; ///< Response timeout for NTP requests
    long minSyncAccuracyUs = DEFAULT_MIN_SYNC_ACCURACY_US;
    uint maxNumSyncRetry = DEFAULT_MAX_RESYNC_RETRY;
    uint numSyncRetry;
    long timeSyncThreshold = DEFAULT_TIME_SYNC_THRESHOLD;
    NTPStatus_t status = unsyncd; ///< Sync status
    char ntpServerName[SERVER_NAME_LENGTH];  ///< Name of NTP server on Internet or LAN
    IPAddress ntpServerIPAddress;            ///< IP address of NTP server on Internet or LAN
#ifdef ESP32
    TaskHandle_t loopHandle = NULL;         ///< TimeSync loop task handle
    TaskHandle_t receiverHandle = NULL;         ///< NTP response receiver task handle
#else
    Ticker loopTimer;           ///< Timer to trigger timesync
    Ticker receiverTimer;           ///< Timer to check received responses
#endif
    Ticker responseTimer;       ///< Timer to trigger response timeout
    bool isConnected = false;
    double offset;
    double delay;
    timezone timeZone;
    char tzname[TZNAME_LENGTH];
    
    int64_t offsetSum;
    int64_t offsetAve;
    uint round = 0;
    uint numAveRounds = DEFAULT_NUM_OFFSET_AVE_ROUNDS;
    
    pbuf* lastNtpResponsePacket;
    bool responsePacketValid = false;
    
    /**
    * Function that gets time from NTP server and convert it to Unix time format
    * @param[in]  NTPClient instance
    */
    static void s_getTimeloop (void* arg);
    
    /**
    * Static method for recvPacket.
    */
    static void s_recvPacket (void* arg, struct udp_pcb* pcb, struct pbuf* p,
                              const ip_addr_t* addr, u16_t port);
    
    static void s_receiverTask (void* arg);
    
    /**
    * Starts a NTP time request to server. Returns a time in UNIX time format. Normally only called from library.
    * Kept in public section to allow direct NTP request.
    */
    void getTime ();
    
    /**
    * Static method for Ticker.
    */
    static void s_processRequestTimeout (void* arg);
    
    /**
    * Process internal state in case of a response timeout. If a response comes later is is asumed as non valid.
    */
    void processRequestTimeout ();
    
    /**
    * Send NTP request to server
    * @param[out] false in case of any error.
    */
    boolean sendNTPpacket ();
    
       
    /**
    * Get packet response and update time as of its data
    * @param[in] UDP response packet.
    */
    void processPacket (struct pbuf* p);
    
    /**
    * Decode NTP response contained in buffer.
    * @param[in] Pointer to message buffer.
    * @param[in] Buffer len.
    * @param[out] Decoded facket from message.
    */
    NTPPacket_t* decodeNtpMessage (uint8_t* messageBuffer, size_t len, NTPPacket_t* decPacket);

    timeval calculateOffset (NTPPacket_t* ntpPacket);
    
    bool adjustOffset (timeval* offset);

public:
    /**
    * NTP client Class destructor
    */
    ~NTPClient () {
        /// stop ();
    }

    /**
    * Starts time synchronization.
    * @param[in] UDP connection instance (optional).
    * @param[in] NTP server name as String.
    * @param[out] true if everything went ok.
    */
    bool begin (const char* ntpServerName = DEFAULT_NTP_SERVER);
    
    /**
    * Sets NTP server name.
    * @param[in] New NTP server name.
    * @param[out] True if everything went ok.
    */
    bool setNtpServerName (const char* serverName);
    
        /**
    * Set a callback that triggers after a sync trial.
    * @param[in] function with void(NTPSyncEvent_t) or std::function<void(NTPSyncEvent_t)> (only for ESP8266)
    *				NTPSyncEvent_t equals 0 is there is no error
    */
    void onNTPSyncEvent (onSyncEvent_t handler){
        if (handler){
            onSyncEvent = handler;
        }
    }
    
    /**
    * Changes sync period.
    * @param[in] New interval in seconds.
    * @param[out] True if everything went ok.
    */
    bool setInterval (int interval);
    
    /**
    * Changes sync period in sync'd and not sync'd status.
    * @param[in] New interval while time is not first adjusted yet, in seconds.
    * @param[in] New interval for normal operation, in seconds.
    * @param[out] True if everything went ok.
    */
    bool setInterval (int shortInterval, int longInterval);
    
    /**
    * Gets sync period.
    * @param[out] Interval for normal operation, in seconds.
    */
    int getInterval () {
        return actualInterval / 1000;
    }

    /**
    * Changes sync period not sync'd status.
    * @param[out] Interval while time is not first adjusted yet, in seconds.
    */
    int	getShortInterval () {
        return shortInterval / 1000;
    }

    /**
    * Gets sync period.
    * @param[out] Interval for normal operation in seconds.
    */
    int	getLongInterval () { 
        return longInterval / 1000;
    }

    /**
    * Sets minimum sync accuracy to get a new request if offset is greater than this value
    * @param[in] Desired minimum accuracy
    */
    void setMinSyncAccuracy (long accuracy) {
        const int minAccuracy = 100;

        if (accuracy > minAccuracy) {
            minSyncAccuracyUs = accuracy;
        } else {
            minSyncAccuracyUs = minAccuracy;
        }
    }
    
    /**
    * Sets time sync threshold. If offset is under this value time will not be adjusted
    * @param[in] Desired sync threshold
    */
    void settimeSyncThreshold (long threshold) {
        const int minThreshold = 100;
        
        if (threshold > minThreshold) {
            timeSyncThreshold = threshold;
        } else {
            timeSyncThreshold = minThreshold;
        }
    }
    
    /**
    * Sets max number of sync retrials if minimum accuracy has not been reached
    * @param[in] MAx sync retrials number
    */
    void setMaxNumSyncRetry (unsigned long maxRetry) {
        if (maxRetry > DEFAULT_MAX_RESYNC_RETRY) {
            numSyncRetry = maxRetry;
        }
    }

    /**
    * Configure response timeout for NTP requests.
    * @param[out] error code. false if faulty.
    */
    boolean setNTPTimeout (uint16_t milliseconds);

    /**
    * Sets time zone for getting local time
    * @param[in] Time zone description
    */
    void setTimeZone (const char* TZ){
        strncpy (tzname, TZ, TZNAME_LENGTH);
        setenv ("TZ", tzname, 1);
        tzset ();
    }
    
    /**
    * Convert current time to a String.
    * @param[out] String constructed from current time.
    */
    char* getTimeStr () {
        time_t currentTime = time (NULL);
        return getTimeStr (currentTime);
    }

    /**
    * Convert a time in UNIX format to a String representing time.
    * @param[in] timeval object to convert to extract time.
    * @param[out] String constructed from current time.
    */
    char* getTimeStr (timeval moment) {
        tm* local_tm = localtime (&moment.tv_usec);
        size_t index = strftime (strBuffer, sizeof (strBuffer), "%H:%M:%S", local_tm);
        snprintf (strBuffer + index, sizeof (strBuffer) - index, ".%06ld", moment.tv_usec);
        return strBuffer;
    }
    
    /**
    * Convert a time in UNIX format to a String representing time.
    * @param[in] time_t object to convert to extract time.
    * @param[out] String constructed from current time.
    */
    char* getTimeStr (time_t moment) {
        tm* local_tm = localtime (&moment);
        strftime (strBuffer, sizeof(strBuffer), "%H:%M:%S", local_tm);

        return strBuffer;
    }

    /**
    * Convert current date to a String.
    * @param[out] String constructed from current date.
    * TODO: Add internationalization support
    */
    char* getDateStr () {
        time_t currentTime = time (NULL);
        return getDateStr (currentTime);
    }

    /**
    * Convert a time in UNIX format to a String representing its date.
    * @param[in] timeval object to convert to extract date.
    * @param[out] String constructed from current date.
    */
    char* getDateStr (timeval moment) {
        return getDateStr (moment.tv_sec);
    }
    
    /**
    * Convert a time in UNIX format to a String representing its date.
    * @param[in] time_t object to convert to extract date.
    * @param[out] String constructed from current date.
    */
    char* getDateStr (time_t moment) {
        tm* local_tm = localtime (&moment);
        strftime (strBuffer, sizeof (strBuffer), "%02d/%m/%04Y", local_tm);

        return strBuffer;
    }
    
    /**
    * Convert current time and date to a String.
    * @param[out] Char string constructed from current time.
    */
    char* getTimeDateString () {
        time_t currentTime = time (NULL);
        return getTimeDateString (currentTime);
    }

    /**
    * Convert current time and date to a String.
    * @param[out] Char string constructed from current time.
    */
    char* getTimeDateStringUs () {
        timeval currentTime;
        gettimeofday (&currentTime, NULL);
        return getTimeDateString (currentTime);
    }
    
    /**
    * Convert given time and date to a String.
    * @param[in] timeval object to convert to String.
    * @param[out] String constructed from current time.
    */
    char* getTimeDateString (timeval moment) {
        tm* local_tm = localtime (&moment.tv_sec);
        size_t index = strftime (strBuffer, sizeof (strBuffer), "%02d/%02m/%04Y %02H:%02M:%02S", local_tm);
        index += snprintf (strBuffer + index, sizeof (strBuffer) - index, ".%06ld", moment.tv_usec);
        strftime (strBuffer + index, sizeof (strBuffer) - index, " %Z", local_tm);
        return strBuffer;
    }

    /**
    * Convert given time and date to a String.
    * @param[in] time_t object to convert to String.
    * @param[out] String constructed from current time.
    */
    char* getTimeDateString (time_t moment) {
        tm* local_tm = localtime (&moment);
        strftime (strBuffer, sizeof (strBuffer), "%02d/%02m/%04Y %02H:%02M:%02S", local_tm);

        return strBuffer;
    }
    
    /**
    * Gets last successful sync time in UNIX format.
    * @param[out] Last successful sync time. 0 equals never.
    */
    timeval getLastNTPSyncUs () {
        return lastSyncd;
    }

    
    /**
    * Gets last successful sync time in UNIX format.
    * @param[out] Last successful sync time. 0 equals never.
    */
    time_t getLastNTPSync (){
        return lastSyncd.tv_sec;
    }

    /**
    * Get uptime in human readable String format.
    * @param[out] Uptime.
    */
    char* getUptimeString ();

    /**
    * Get uptime in UNIX format, time since MCU was last rebooted.
    * @param[out] Uptime. 0 equals never.
    */
    time_t getUptime () {
        uptime = uptime + (::millis () - uptime);
        return uptime / 1000;
    }

    /**
    * Get first successful synchronization time after boot.
    * @param[out] First sync time.
    */
    timeval getFirstSyncUs () {
        return firstSync;
    }
    
    /**
    * Get first successful synchronization time after boot.
    * @param[out] First sync time.
    */
    time_t getFirstSync () {
        // if (!firstSync) {
        //     if (timeStatus () == timeSet) {
        //         firstSync = time (NULL) - getUptime ();
        //     }
        // }
        return firstSync.tv_sec;
    }
    
    /**
    * Returns sync status
    * @param[out] Sync status as NTPStatus_t
    */
    NTPStatus_t syncStatus () {
        return status;
    }

    /**
    * Gets NTP server name
    * @param[out] NTP server name.
    */
    char* getNtpServerName (){
        return ntpServerName;
    }
    
    int64_t millis () {
        timeval currentTime;
        gettimeofday (&currentTime, NULL);
        int64_t milliseconds = (int64_t)currentTime.tv_sec * 1000L + (int64_t)currentTime.tv_usec / 1000L;
        //Serial.printf ("timeval: %ld.%ld millis %lld\n", currentTime.tv_sec, currentTime.tv_usec, milliseconds);
        return milliseconds;
    }
    
    int64_t micros() {
        timeval currentTime;
        gettimeofday (&currentTime, NULL);
        int64_t microseconds = (int64_t)currentTime.tv_sec * 1000000L + (int64_t)currentTime.tv_usec;
        //Serial.printf ("timeval: %ld.%ld micros %lld\n", currentTime.tv_sec, currentTime.tv_usec, microseconds);
        return microseconds;
    }

    char* ntpEvent2str (NTPEvent_t e);

    
};

extern NTPClient NTP;




#endif // _NtpClientLib_h
