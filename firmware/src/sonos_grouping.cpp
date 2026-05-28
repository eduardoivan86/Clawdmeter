#include "sonos_grouping.h"
#include "secrets.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <esp_log.h>
#include <pgmspace.h>

static const char* TAG = "sonos-group";

static const char SOAP_JOIN_BODY[] PROGMEM =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "<CurrentURI>x-rincon:%s</CurrentURI>"
    "<CurrentURIMetaData></CurrentURIMetaData>"
    "</u:SetAVTransportURI>"
    "</s:Body></s:Envelope>";

static const char SOAP_LEAVE_BODY[] PROGMEM =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>"
    "<u:BecomeCoordinatorOfStandaloneGroup xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID>"
    "</u:BecomeCoordinatorOfStandaloneGroup>"
    "</s:Body></s:Envelope>";

static const char SOAP_ACTION_JOIN[]  =
    "\"urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI\"";
static const char SOAP_ACTION_LEAVE[] =
    "\"urn:schemas-upnp-org:service:AVTransport:1#BecomeCoordinatorOfStandaloneGroup\"";

static bool soap_post(const char* ip, const char* path,
                      const char* soap_action,
                      const char* body) {
    HTTPClient http;
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d%s", ip, SONOS_PORT, path);
    if (!http.begin(url)) {
        ESP_LOGE(TAG, "http.begin failed: %s", url);
        return false;
    }
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPACTION", soap_action);
    http.setTimeout(10000);
    int code = http.POST((uint8_t*)body, strlen(body));
    http.end();
    if (code >= 200 && code < 300) {
        ESP_LOGI(TAG, "%s -> %d ok", soap_action, code);
        return true;
    }
    ESP_LOGE(TAG, "%s -> %d FAIL", soap_action, code);
    return false;
}

bool sonos_grouping_join(const char* slave_ip, const char* coordinator_rincon) {
    char body[600];
    int n = snprintf_P(body, sizeof(body), SOAP_JOIN_BODY, coordinator_rincon);
    if (n < 0 || n >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "join body too large (%d)", n);
        return false;
    }
    return soap_post(slave_ip, "/MediaRenderer/AVTransport/Control",
                     SOAP_ACTION_JOIN, body);
}

bool sonos_grouping_leave(const char* slave_ip) {
    char body[500];
    int n = snprintf_P(body, sizeof(body), SOAP_LEAVE_BODY);
    if (n < 0 || n >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "leave body too large (%d)", n);
        return false;
    }
    return soap_post(slave_ip, "/MediaRenderer/AVTransport/Control",
                     SOAP_ACTION_LEAVE, body);
}
