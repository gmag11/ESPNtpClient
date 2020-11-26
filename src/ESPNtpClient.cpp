#include "ESPNtpClient.h"


#define DBG_PORT Serial

#ifdef DEBUG_NTPCLIENT

#ifdef ESP8266
const char* extractFileName (const char* path);
#define DEBUG_LINE_PREFIX() DBG_PORT.printf_P (PSTR("[%lu][%s:%d] %s() Heap: %lu | "),millis(),extractFileName(__FILE__),__LINE__,__FUNCTION__,(unsigned long)ESP.getFreeHeap())
#define DEBUGLOG(text,...) DEBUG_LINE_PREFIX();DBG_PORT.printf_P(PSTR(text),##__VA_ARGS__);DBG_PORT.println()
#elif defined ESP32
#define ARDUHAL_NTP_LOG(format)  "[%s:%u] %d %s(): " format "\r\n", pathToFileName(__FILE__), __LINE__, (unsigned long)ESP.getFreeHeap(), __FUNCTION__
#define DEBUGLOG(format, ...) log_printf(ARDUHAL_NTP_LOG(format), ##__VA_ARGS__)
#else
#define DEBUGLOG(...) DBG_PORT.printf(__VA_ARGS__);DBG_PORT.println();
#endif // ESP8266
#else
#define DEBUGLOG(...)
#endif // DEBUG_NTPCLIENT

#ifdef ESP8266
const char* IRAM_ATTR extractFileName (const char* path) {
    size_t i = 0;
    size_t pos = 0;
    char* p = (char*)path;
    while (*p) {
        i++;
        if (*p == '/' || *p == '\\') {
            pos = i;
        }
        p++;
    }
    return path + pos;
}
#endif

typedef struct {
    int32_t secondsOffset;
    uint32_t fraction;
} timestamp_t;

typedef struct __attribute__ ((packed, aligned (1))) {
    uint8_t flags;
    uint8_t peerStratum;
    uint8_t pollingInterval;
    uint8_t clockPrecission;
    uint32_t rootDelay;
    uint32_t dispersion;
    uint8_t refID[4];
    timestamp_t reference;
    timestamp_t origin;
    timestamp_t receive;
    timestamp_t transmit;
} NTPUndecodedPacket_t;

NTPClient NTP;

const int seventyYears = 2208988800UL; // From 1900 to 1970

int32_t flipInt32 (int32_t number) {
    uint8_t output[sizeof (int32_t)];
    uint8_t* input = (uint8_t*)&number;
    //DEBUGLOG ("Input number %08X", number);

    for (unsigned int i = 1; i <= sizeof (int32_t); i++) {
        output[i - 1] = input[sizeof (int32_t) - i];
    }

    //DEBUGLOG ("Output number %08X", *(int32_t*)output);
    int32_t *result = (int32_t*)output;
    return *result;
}

char* dumpNTPPacket (byte* data, size_t length, char* buffer, int len) {
    //byte *data = packet.data ();
    //size_t length = packet.length ();
    int remaining = len - 1;
    int index = 0;
    int written;

    for (size_t i = 0; i < length; i++) {
        if (remaining > 0) {
            written = snprintf (buffer + index, remaining, "%02X ", data[i]);
            index += written;
            remaining -= written;
            if ((i + 1) % 16 == 0) {
                written = snprintf (buffer + index, remaining, "\n");
                index += written;
                remaining -= written;
            } else if ((i + 1) % 4 == 0) {
                written = snprintf (buffer + index, remaining, "| ");
                index += written;
                remaining -= written;
            }
        }
    }
    return buffer;
}

bool NTPClient::begin (const char* ntpServerName) {
    err_t result;
    
    if (!setNtpServerName (ntpServerName) || !strnlen (ntpServerName, SERVER_NAME_LENGTH)) {
        DEBUGLOG ("Invalid NTP server name");
        return false;
    }

    udp = udp_new ();
    if (!udp){
        DEBUGLOG ("Failed to create NTP socket");
        return false;
    }
    
    if (WiFi.isConnected ()) {
        ip_addr_t localAddress;
#ifdef ESP32
        localAddress.u_addr.ip4.addr = WiFi.localIP ();
        localAddress.type = IPADDR_TYPE_V4;
#else
        localAddress.addr = WiFi.localIP ();
#endif
        result = udp_bind (udp, &localAddress, DEFAULT_NTP_PORT);
        if (result) {
            DEBUGLOG ("Failed to bind to NTP port. %d", result);
            return false;
        }

        udp_recv (udp, &NTPClient::s_recvPacket, this);
        isConnected = true;
    }
    lastSyncd.tv_sec = 0;
    lastSyncd.tv_usec = 0;
    
    DEBUGLOG ("Time sync started");
    
    //Start loop() task
#ifdef ESP32
    xTaskCreateUniversal (
        &NTPClient::s_getTimeloop, /* Task function. */
        "NTP receiver", /* name of task. */
        4096, /* Stack size of task */
        this, /* parameter of the task */
        1, /* priority of the task */
        &loopHandle, /* Task handle to keep track of created task */
        CONFIG_ARDUINO_RUNNING_CORE);
#else
    loopTimer.attach_ms (ESP8266_LOOP_TASK_INTERVAL, &NTPClient::s_getTimeloop, (void*)this);
#endif
    
    DEBUGLOG ("First time sync request");
    getTime ();
    
    return true;

}

void NTPClient::processPacket (struct pbuf* packet) {
    NTPPacket_t ntpPacket;
    bool offsetApplied = false;
    static bool wasPartial;
    
    if (!packet) {
        DEBUGLOG ("Received packet empty");
    }
    DEBUGLOG ("Data lenght %d", packet->len);

    if (status != ntpRequested) {
        DEBUGLOG ("Unrequested response");
        return;
    }
    
    if (packet->len < NTP_PACKET_SIZE) {
        DEBUGLOG ("Response Error");
        status = unsyncd;
        // actualInterval = shortInterval;
        // DEBUGLOG ("Set interval to = %d", actualInterval);
        // DEBUGLOG ("Status set to UNSYNCD");
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = responseError;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            event.info.offset = 0;
            event.info.delay = 0;
            onSyncEvent (event);
        }  
        return;
    }

    responseTimer.detach ();
    
    decodeNtpMessage ((uint8_t*)packet->payload, packet->len, &ntpPacket);
    timeval tvOffset = calculateOffset (&ntpPacket);
    if (tvOffset.tv_sec == 0 && abs (tvOffset.tv_usec) < TIME_SYNC_THRESHOLD) { // Less than 1 ms
        DEBUGLOG ("Offset %0.3f ms is under threshold. Not updating", ((float)tvOffset.tv_sec + (float)tvOffset.tv_usec / 1000000.0) * 1000);
    } else {
        if (!adjustOffset (&tvOffset)) {
            DEBUGLOG ("Error applying offset");
            if (onSyncEvent) {
                NTPEvent_t event;
                event.event = syncError;
                event.info.serverAddress = ntpServerIPAddress;
                event.info.port = DEFAULT_NTP_PORT;
                event.info.offset = ((float)tvOffset.tv_sec + (float)tvOffset.tv_usec / 1000000.0) * 1000.0;
                onSyncEvent (event);
            }
        }
        offsetApplied = true;

    }
    if (tvOffset.tv_sec != 0 || abs (tvOffset.tv_usec) > MIN_SYNC_ACCURACY_US) { // Offset bigger than 10 ms
        DEBUGLOG ("Minimum accuracy not reached. Repeating sync");
        DEBUGLOG ("Next sync programmed for %d ms", FAST_NTP_SYNCNTERVAL);
        status = partialSync;
        wasPartial = true;
    } else {
        DEBUGLOG ("Status set to SYNCD");
        DEBUGLOG ("Next sync programmed for %d seconds", getLongInterval ());
        status = syncd;    
        if (wasPartial){
            offsetApplied = true;
        }
        //Serial.printf ("Status: %d wasPartial: %d offsetApplied %d Offset %0.3f\n", status, wasPartial, offsetApplied, ((float)tvOffset.tv_sec + (float)tvOffset.tv_usec / 1000000.0) * 1000);
        wasPartial = false;
    }
    if (status == partialSync) {
        actualInterval = FAST_NTP_SYNCNTERVAL;
    } else {
        actualInterval = longInterval;
    }
    DEBUGLOG ("Interval set to = %d", actualInterval);
    DEBUGLOG ("Sync frequency set low");
    DEBUGLOG ("Successful NTP sync at %s", getTimeDateString (getLastNTPSync ()));
    if (!firstSync.tv_sec) {
        firstSync = lastSyncd;
    }
    if (offsetApplied && onSyncEvent) {
        NTPEvent_t event;
        if (status == partialSync){
            event.event = partlySync;
        } else {
            event.event = timeSyncd;
        }
        event.info.offset = offset;
        event.info.delay = delay;
        event.info.serverAddress = ntpServerIPAddress;
        event.info.port = DEFAULT_NTP_PORT;
        onSyncEvent (event);
    }
    // const int sizeStr = 200;
    // char strBuffer[sizeStr];
    // DEBUGLOG ("NTP Packet\n%s", dumpNTPPacket ((byte*)(packet->payload), packet->len, strBuffer, sizeStr));
    pbuf_free (packet);

}

void NTPClient::s_recvPacket (void* arg, struct udp_pcb* pcb, struct pbuf* p,
                              const ip_addr_t* addr, u16_t port) {
    NTPPacket_t ntpPacket;
    
    NTPClient* self = reinterpret_cast<NTPClient*>(arg);
    gettimeofday (&(self->packetLastReceived), NULL);
    DEBUGLOG ("NTP Packet received from %s:%d", ipaddr_ntoa (addr), port);
    self->processPacket (p);
}

char* NTPClient::getUptimeString () {
    uint16_t days;
    uint8_t hours;
    uint8_t minutes;
    uint8_t seconds;
    //static char strbuffer[28];

    time_t uptime = getUptime ();

    seconds = uptime % SECS_PER_MIN;
    uptime -= seconds;
    minutes = (uptime % SECS_PER_HOUR) / SECS_PER_MIN;
    uptime -= minutes * SECS_PER_MIN;
    hours = (uptime % SECS_PER_DAY) / SECS_PER_HOUR;
    uptime -= hours * SECS_PER_HOUR;
    days = uptime / SECS_PER_DAY;

    snprintf (strBuffer, sizeof (strBuffer) - 1, "%4u days %02d:%02d:%02d", days, hours, minutes, seconds);

    return strBuffer;
}

void NTPClient::s_getTimeloop (void* arg) {
#ifdef ESP32
    for (;;) {
#endif
        //DEBUGLOG ("Running periodic task");
        NTPClient* self = reinterpret_cast<NTPClient*>(arg);
        static time_t lastGotTime;

        if (self->isConnected) {
            if (WiFi.isConnected()) {
                if (millis () - lastGotTime >= self->actualInterval) {
                    lastGotTime = millis ();
                    DEBUGLOG ("Periodic loop. Millis = %d", lastGotTime);
                    self->getTime ();  
                }
            } else {
                DEBUGLOG ("DISCONNECTED");
                udp_remove (self->udp);
                self->isConnected = false;
            }
        } else {
            if (WiFi.isConnected ()){
                DEBUGLOG ("CONNECTED. Binding");
                
                self->udp = udp_new ();
                if (!self->udp) {
                    DEBUGLOG ("Failed to create NTP socket");
                    return; // false;
                }
                
                ip_addr_t localAddress;
#ifdef ESP32
                localAddress.u_addr.ip4.addr = WiFi.localIP ();
                localAddress.type = IPADDR_TYPE_V4;
#else
                localAddress.addr = WiFi.localIP ();
#endif
                err_t result = udp_bind (self->udp, &localAddress, DEFAULT_NTP_PORT);
                if (result) {
                    DEBUGLOG ("Failed to bind to NTP port. %d", result);
                    //return; //false;
                }

                udp_recv (self->udp, &NTPClient::s_recvPacket, self);
                self->getTime ();
                self->isConnected = true;
            }
        }
    }
#ifdef ESP32
}
#endif

void NTPClient::getTime () {
    err_t result;
    
    result = WiFi.hostByName (getNtpServerName (), ntpServerIPAddress);
    if (!result) {
        DEBUGLOG ("HostByName error %d", (int)result);

        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = invalidAddress;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;

            onSyncEvent (event);        
        }
    }
    if (ntpServerIPAddress == INADDR_NONE) {
        DEBUGLOG ("IP address unset. Aborting");
        actualInterval = shortInterval;
        DEBUGLOG ("Set interval to = %d", actualInterval);
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = invalidAddress;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            onSyncEvent (event);
        }
        return;
    }
    
    ip_addr ntpAddr;
#ifdef ESP32
    ntpAddr.type = IPADDR_TYPE_V4;
    ntpAddr.u_addr.ip4.addr = ntpServerIPAddress;
#else
    ntpAddr.addr = ntpServerIPAddress;
#endif
    DEBUGLOG ("NTP server IP address %s", ipaddr_ntoa (&ntpAddr));
    result = udp_connect (udp, &ntpAddr, DEFAULT_NTP_PORT);
    if (result == ERR_USE) {
        DEBUGLOG ("Port already used");
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = invalidPort;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            onSyncEvent (event);
        }
        //return;
    }
    if (result == ERR_RTE) {
        DEBUGLOG ("Port already used");
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = invalidAddress;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            onSyncEvent (event);
        }
        //return;
    }
    
    DEBUGLOG ("Sending UDP packet");
    NTPStatus_t prevStatus = status;
    status = ntpRequested;
    DEBUGLOG ("Status set to REQUESTED");
    responseTimer.once_ms (ntpTimeout, &NTPClient::s_processRequestTimeout, static_cast<void*>(this));
    
    if (!sendNTPpacket ()) {
        responseTimer.detach ();
        DEBUGLOG ("NTP request error");
        status = prevStatus;
        DEBUGLOG ("Status recovered due to UDP send error");
        if (onSyncEvent) {
            NTPEvent_t event;
            event.event = errorSending;
            event.info.serverAddress = ntpServerIPAddress;
            event.info.port = DEFAULT_NTP_PORT;
            onSyncEvent (event);
        }
        return;
    }
    if (onSyncEvent) {
        NTPEvent_t event;
        event.event = requestSent;
        event.info.serverAddress = ntpServerIPAddress;
        event.info.port = DEFAULT_NTP_PORT;
        onSyncEvent (event);
    }
    udp_disconnect (udp);
    
}

boolean NTPClient::sendNTPpacket () {
    err_t result;
    timeval currentime;
    pbuf* buffer;
    NTPUndecodedPacket_t packet;
    buffer = pbuf_alloc (PBUF_TRANSPORT, sizeof (NTPUndecodedPacket_t), PBUF_RAM);
    if (!buffer){
        DEBUGLOG ("Cannot allocate UDP packet buffer");
        return false;
    }
    buffer->len = sizeof (NTPUndecodedPacket_t);
    buffer->tot_len = sizeof (NTPUndecodedPacket_t);

    memset (&packet, 0, sizeof (NTPUndecodedPacket_t));
    
    packet.flags = 0b11100011;
    packet.peerStratum = 0;
    packet.pollingInterval = 6;
    packet.clockPrecission = 0xEC;

    gettimeofday (&currentime, NULL);
    
    if (currentime.tv_sec != 0) {
        packet.transmit.secondsOffset = flipInt32 (currentime.tv_sec + seventyYears);
        DEBUGLOG ("Current time: %ld.%ld", currentime.tv_sec, currentime.tv_usec);
        uint32_t timestamp_us = (double)(currentime.tv_usec) / 1000000.0 * (double)0x100000000;
        DEBUGLOG ("timestamp_us = %d", timestamp_us);
        packet.transmit.fraction = flipInt32 (timestamp_us);
        DEBUGLOG ("Transmit: %d : %d", packet.transmit.secondsOffset, packet.transmit.fraction);
        
    } else {
        packet.transmit.secondsOffset = 0;
        packet.transmit.fraction = 0;
    }

#ifdef DEBUG_NTPCLIENT
    const int sizeStr = 200;
    char strPacketBuffer[sizeStr];
    DEBUGLOG ("NTP Packet\n%s", dumpNTPPacket ((uint8_t*)&packet, sizeof (NTPUndecodedPacket_t), strPacketBuffer, sizeStr));
#endif

    memcpy (buffer->payload, &packet, sizeof (NTPUndecodedPacket_t));
    result = udp_send (udp, buffer);
    pbuf_free (buffer);
    if (result == ERR_OK) {
        DEBUGLOG ("UDP packet sent");
        return true;
    } else {
        return false;
    }
}

void ICACHE_RAM_ATTR NTPClient::s_processRequestTimeout (void* arg) {
    NTPClient* self = reinterpret_cast<NTPClient*>(arg);
    self->processRequestTimeout ();
}

void ICACHE_RAM_ATTR NTPClient::processRequestTimeout () {
    status = unsyncd;
    DEBUGLOG ("Status set to UNSYNCD");
    //timer1_disable ();
    responseTimer.detach ();
    DEBUGLOG ("NTP response Timeout");
    if (onSyncEvent) {
        NTPEvent_t event;
        event.event = noResponse;
        event.info.serverAddress = ntpServerIPAddress;
        event.info.port = DEFAULT_NTP_PORT;
        onSyncEvent (event);
    }
    // if (status==syncd) {
    //     actualInterval = longInterval;
    // } else {
    //     actualInterval = shortInterval;
    // }
    //DEBUGLOG ("Set interval to = %d", actualInterval);
}

bool NTPClient::setNtpServerName (const char* serverName) {
    if (!serverName) {
        return false;
    }
    if (!strlen (serverName)) {
        return false;
    }
    DEBUGLOG ("NTP server set to %s\n", serverName);
    memset (ntpServerName, 0, SERVER_NAME_LENGTH);
    strncpy (ntpServerName, serverName, strnlen (serverName, SERVER_NAME_LENGTH));
    return true;
}

bool NTPClient::setInterval (int interval) {
    unsigned int newInterval = interval * 1000;
    if (interval >= MIN_NTP_INTERVAL) {
        if (longInterval != newInterval) {
            longInterval = newInterval;
            DEBUGLOG ("Sync interval set to %d s", interval);
            if (syncStatus () == syncd) {
                actualInterval = longInterval;
                DEBUGLOG ("Set interval to = %d", actualInterval);
            }
        }
        return true;
    } else {
        longInterval = MIN_NTP_INTERVAL * 1000;
        if (syncStatus () == syncd) {
            actualInterval = longInterval;
            DEBUGLOG ("Set interval to = %d", actualInterval);
        }
        DEBUGLOG ("Too low value. Sync interval set to minimum: %d s", MIN_NTP_INTERVAL);
        return false;
    }
}

bool NTPClient::setInterval (int shortInterval, int longInterval) {
    int newShortInterval = shortInterval * 1000;
    int newLongInterval = longInterval * 1000;
    if (shortInterval >= MIN_NTP_INTERVAL && longInterval >= MIN_NTP_INTERVAL) {
        this->shortInterval = newShortInterval;
        this->longInterval = newLongInterval;
        if (syncStatus () != syncd) {
            actualInterval = this->shortInterval;

        } else {
            actualInterval = this->longInterval;
        }
        DEBUGLOG ("Interval set to = %d", actualInterval);
        DEBUGLOG ("Short sync interval set to %d s\n", shortInterval);
        DEBUGLOG ("Long sync interval set to %d s\n", longInterval);
return true;
    } else {
        DEBUGLOG ("Too low interval values");
        return false;    
    }
}


boolean NTPClient::setNTPTimeout (uint16_t milliseconds) {

    if (milliseconds >= MIN_NTP_TIMEOUT) {
        ntpTimeout = milliseconds;
        DEBUGLOG ("Set NTP timeout to %u ms", milliseconds);
        return true;
    }
    DEBUGLOG ("NTP timeout should be higher than %u ms. You've tried to set %u ms", MIN_NTP_TIMEOUT, milliseconds);
    return false;

}

NTPPacket_t* NTPClient::decodeNtpMessage (uint8_t* messageBuffer, size_t length, NTPPacket_t* decPacket) {
    NTPUndecodedPacket_t recPacket;
    int32_t timestamp_s;
    uint32_t timestamp_us;

    if (length < NTP_PACKET_SIZE) {
        return NULL;
    }

    memcpy (&recPacket, messageBuffer, NTP_PACKET_SIZE);

    DEBUGLOG ("Decoded NTP message");
#ifdef DEBUG_NTPCLIENT
    char buffer[250];
#endif
    DEBUGLOG ("\n%s", dumpNTPPacket (messageBuffer, length, buffer, 250));

    decPacket->flags.li = recPacket.flags >> 6;
    DEBUGLOG ("LI = %u", decPacket->flags.li);

    decPacket->flags.vers = recPacket.flags >> 3 & 0b111;
    DEBUGLOG ("Version = %u", decPacket->flags.vers);

    decPacket->flags.mode = recPacket.flags & 0b111;
    DEBUGLOG ("Mode = %u", decPacket->flags.mode);

    decPacket->peerStratum = recPacket.peerStratum;
    DEBUGLOG ("Peer Stratum = %u", decPacket->peerStratum);

    decPacket->pollingInterval = recPacket.pollingInterval;
    DEBUGLOG ("Polling Interval = 0x%02X", decPacket->pollingInterval);

    decPacket->clockPrecission = recPacket.clockPrecission;
    DEBUGLOG ("Clock Precission = 0x%02X", decPacket->clockPrecission);

    decPacket->rootDelay = flipInt32 (recPacket.rootDelay);
    DEBUGLOG ("Root delay: 0x%08X", decPacket->rootDelay);

    decPacket->dispersion = flipInt32 (recPacket.dispersion);
    DEBUGLOG ("Dispersion: 0x%08X", decPacket->dispersion);

    memcpy (&(decPacket->refID), &(recPacket.refID), 4);
    DEBUGLOG ("refID: %u.%u.%u.%u", decPacket->refID[0], decPacket->refID[1], decPacket->refID[2], decPacket->refID[3]);
    DEBUGLOG ("refID: %.*s", 4, (char*)(decPacket->refID));

    // Reference timestamp
    timestamp_s = flipInt32 (recPacket.reference.secondsOffset);
    timestamp_us = flipInt32 (recPacket.reference.fraction);
    //DEBUGLOG ("timestamp_us %08X = %lu", timestamp_us, timestamp_us);
    if (timestamp_s) {
        decPacket->reference.tv_sec = timestamp_s - seventyYears;
    } else {
        decPacket->reference.tv_sec = 0;
    }
    decPacket->reference.tv_usec = ((float)(timestamp_us) / (float)0x100000000 * 1000000.0);
    //DEBUGLOG ("Reference: seconds %08X fraction %08X", recPacket.reference.secondsOffset, recPacket.reference.fraction);
    //DEBUGLOG ("Reference: %d.%06ld", decPacket->reference.tv_sec, decPacket->reference.tv_usec);
    DEBUGLOG ("Reference: %s.%06ld", ctime (&(decPacket->reference.tv_sec)), decPacket->reference.tv_usec);

    // Origin timestamp
    timestamp_s = flipInt32 (recPacket.origin.secondsOffset);
    timestamp_us = flipInt32 (recPacket.origin.fraction);
    //DEBUGLOG ("timestamp_us %08X = %lu", timestamp_us, timestamp_us);
    if (timestamp_s) {
        decPacket->origin.tv_sec = timestamp_s - seventyYears;
    } else {
        decPacket->origin.tv_sec = 0;
    }
    decPacket->origin.tv_usec = ((float)(timestamp_us) / (float)0x100000000 * 1000000.0);
    //DEBUGLOG ("Origin: seconds %08X fraction %08X", recPacket.origin.secondsOffset, recPacket.origin.fraction);
    //DEBUGLOG ("Origin: %d.%06ld", decPacket->origin.tv_sec, decPacket->origin.tv_usec);
    DEBUGLOG ("Origin: %s.%06ld", ctime (&(decPacket->origin.tv_sec)), decPacket->origin.tv_usec);

    // Receive timestamp
    timestamp_s = flipInt32 (recPacket.receive.secondsOffset);
    timestamp_us = flipInt32 (recPacket.receive.fraction);
    //DEBUGLOG ("timestamp_us %08X = %lu", timestamp_us, timestamp_us);
    if (timestamp_s) {
        decPacket->receive.tv_sec = timestamp_s - seventyYears;
    } else {
        decPacket->receive.tv_sec = 0;
    }
    decPacket->receive.tv_usec = ((float)(timestamp_us) / (float)0x100000000 * 1000000.0);
    //DEBUGLOG ("Receive: seconds %08X fraction %08X", recPacket.receive.secondsOffset, recPacket.receive.fraction);
    //DEBUGLOG ("Receive: %d.%06ld", decPacket->receive.tv_sec, decPacket->receive.tv_usec);
    DEBUGLOG ("Receive: %s.%06ld", ctime (&(decPacket->receive.tv_sec)), decPacket->receive.tv_usec);

    // Transmit timestamp
    timestamp_s = flipInt32 (recPacket.transmit.secondsOffset);
    timestamp_us = flipInt32 (recPacket.transmit.fraction);
    //DEBUGLOG ("timestamp_us %08X = %lu", timestamp_us, timestamp_us);

    if (timestamp_s) {
        decPacket->transmit.tv_sec = timestamp_s - seventyYears;
    } else {
        decPacket->transmit.tv_sec = 0;
    }
    decPacket->transmit.tv_usec = ((float)(timestamp_us) / (float)0x100000000 * 1000000.0);
    //DEBUGLOG ("Transmit: seconds %08X fraction %08X", recPacket.transmit.secondsOffset, recPacket.transmit.fraction);
    //DEBUGLOG ("Transmit: %d.%06ld", decPacket->transmit.tv_sec, decPacket->transmit.tv_usec);
    DEBUGLOG ("Transmit: %s.%06ld", ctime (&(decPacket->transmit.tv_sec)), decPacket->transmit.tv_usec);

    return decPacket;
}

timeval NTPClient::calculateOffset (NTPPacket_t* ntpPacket) {
    double t1, t2, t3, t4;
    timeval tv_offset;
    //timeval currenttime;

    t1 = ntpPacket->origin.tv_sec + ntpPacket->origin.tv_usec / 1000000.0;
    t2 = ntpPacket->receive.tv_sec + ntpPacket->receive.tv_usec / 1000000.0;
    t3 = ntpPacket->transmit.tv_sec + ntpPacket->transmit.tv_usec / 1000000.0;
    //gettimeofday (&currenttime,NULL);
    t4 = packetLastReceived.tv_sec + packetLastReceived.tv_usec / 1000000.0;
    offset = ((t2 - t1) / 2.0 + (t3 - t4) / 2.0); // in seconds
    delay = (t4 - t1) - (t3 - t2); // in seconds

    DEBUGLOG ("T1: %f T2: %f T3: %f T4: %f", t1, t2, t3, t4);
    DEBUGLOG ("T1: %s", getTimeDateString (ntpPacket->origin));
    //DEBUGLOG ("T1: %016X  %016X", ntpPacket->origin.tv_sec, ntpPacket->origin.tv_usec);
    DEBUGLOG ("T2: %s", getTimeDateString (ntpPacket->receive));
    //DEBUGLOG ("T2: %016X  %016X", ntpPacket->receive.tv_sec, ntpPacket->receive.tv_usec);
    DEBUGLOG ("T3: %s", getTimeDateString (ntpPacket->transmit));
    //DEBUGLOG ("T3: %016X  %016X", ntpPacket->transmit.tv_sec, ntpPacket->transmit.tv_usec);
    DEBUGLOG ("T4: %s", getTimeDateString (packetLastReceived));
    //DEBUGLOG ("T4: %016X  %016X", packetLastReceived.tv_sec, packetLastReceived.tv_usec);
    DEBUGLOG ("Offset: %f, Delay: %f", offset, delay);

    //DEBUGLOG ("Current time: %d.%d", currenttime.tv_sec, currenttime.tv_usec);
    //DEBUGLOG ("Current time: %s", ctime (&(currenttime.tv_sec)));

    tv_offset.tv_sec = (time_t)offset;
    tv_offset.tv_usec = (offset - (double)tv_offset.tv_sec) * 1000000.0;

    DEBUGLOG ("Calculated offset %f sec. Delay %f ms", offset, delay * 1000);

    return tv_offset;
}

bool NTPClient::adjustOffset (timeval* offset) {
    timeval newtime;
    timeval currenttime;
    //timeval _offset;

    //_offset.tv_sec = offset->tv_sec;
    //_offset.tv_usec = offset->tv_usec;

    gettimeofday (&currenttime, NULL);

    int64_t currenttime_us = (int64_t)currenttime.tv_sec * 1000000L + (int64_t)currenttime.tv_usec;
    int64_t offset_us = (int64_t)offset->tv_sec * 1000000L + (int64_t)offset->tv_usec;

    // Serial.printf ("currenttime  %ld.%ld\n", currenttime.tv_sec, currenttime.tv_usec);
    // Serial.printf ("currenttime_us: %f\n", currenttime_us);
    // Serial.printf ("offset  %ld.%ld\n", offset->tv_sec, offset->tv_usec);
    // Serial.printf ("offset_us: %f\n", offset_us);

    int64_t newtime_us = currenttime_us + offset_us;

    newtime.tv_sec = newtime_us / 1000000L;
    newtime.tv_usec = newtime_us - ((int64_t)newtime.tv_sec * 1000000L);
    // Serial.printf ("newtime_us: %lld\n", newtime_us);
    // Serial.printf ("newtime  %ld.%ld\n", newtime.tv_sec, newtime.tv_usec);
    
    // if (_offset.tv_usec > 0) {
    //     timeradd (&currenttime, &_offset, &newtime);
    // } else {
    //     _offset.tv_usec = -_offset.tv_usec;
    //     timersub (&currenttime, &_offset, &newtime);
    // }

    if (settimeofday (&newtime, NULL)) { // hard adjustment
        return false;
    }
    //Serial.printf ("millis() offset 1: %lld\n", currenttime_us / 1000 - millis ());
    //Serial.printf ("millis() offset 2: %lld\n", newtime_us / 1000 - millis ());
    DEBUGLOG ("Diferencia: %lld", (newtime_us - currenttime_us));
    //Serial.printf ("Requested offset %ld.%ld\n", offset->tv_sec, offset->tv_usec);
    //Serial.printf ("Requested new time %ld.%ld\n", newtime.tv_sec, newtime.tv_usec);
    //Serial.printf ("Requested new time %s\n", ctime (&(newtime.tv_sec)));

    DEBUGLOG ("Hard adjust");

    lastSyncd = newtime;
    DEBUGLOG ("Offset adjusted");
    return true;
}
