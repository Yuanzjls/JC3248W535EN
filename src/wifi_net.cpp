#include "wifi_net.h"

#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "serial_log.h"

static esp_netif_t *s_netif = nullptr;
static volatile bool s_has_ip = false;
static volatile bool s_auto_reconnect = false; /* set by connect, cleared by our own disconnect */
static volatile bool s_retry_once = false;     /* first failure of a connect: retry once (as Arduino) */

/* Scan: the driver's SCAN_DONE event sets the state; results are fetched on the polling task. */
enum : int { k_scan_idle = 0, k_scan_running, k_scan_done, k_scan_failed };
static volatile int s_scan_state = k_scan_idle;
static std::vector<wifi_ap_record_t> s_scan;

/* Events are logged from wifi_net_poll(): the event loop task has a 2 KB stack, too small for
 * serial_log (vsnprintf + 256 B buffer). */
struct NetEvent {
    int32_t id;
    int32_t a;
    int32_t b;
};
enum : int32_t { ev_start = 1, ev_stop, ev_connected, ev_disconnected, ev_got_ip, ev_lost_ip, ev_reconnect };
static QueueHandle_t s_events = nullptr;

static void post(int32_t id, int32_t a = 0, int32_t b = 0) {
    const NetEvent e = {id, a, b};
    if (s_events) {
        xQueueSend(s_events, &e, 0);
    }
}

static const char *reason_name(int r) {
    switch (r) {
    case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY: return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA: return "NOT_AUTHED";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA: return "NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED";
    case WIFI_REASON_INVALID_PMKID: return "INVALID_PMKID";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET: return "AP_TSF_RESET";
    case WIFI_REASON_ROAMING: return "ROAMING";
    default: return "?";
    }
}

/** Reasons Arduino reconnects after (not ASSOC_LEAVE = our own disconnect, not AUTH_FAIL = bad password). */
static bool reconnectable(int r) {
    switch (r) {
    case WIFI_REASON_UNSPECIFIED:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
    case WIFI_REASON_ASSOC_TOOMANY:
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA:
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA:
    case WIFI_REASON_ASSOC_NOT_AUTHED:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
    case WIFI_REASON_INVALID_PMKID:
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
    case WIFI_REASON_AP_TSF_RESET:
    case WIFI_REASON_ROAMING:
        return true;
    default:
        return false;
    }
}

/* Runs on the default event loop task: no logging here (see post()). */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        post(ev_start);
        break;
    case WIFI_EVENT_STA_STOP:
        post(ev_stop);
        break;
    case WIFI_EVENT_STA_CONNECTED:
        post(ev_connected, ((wifi_event_sta_connected_t *)data)->channel);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        const int reason = d->reason ? d->reason : WIFI_REASON_UNSPECIFIED;
        s_has_ip = false;
        post(ev_disconnected, reason, d->rssi);
        if (reason == WIFI_REASON_ASSOC_LEAVE || !s_auto_reconnect) {
            break; /* our own disconnect */
        }
        if (s_retry_once || reconnectable(reason)) {
            s_retry_once = false;
            post(ev_reconnect, reason);
            esp_wifi_connect();
        }
        break;
    }
    case WIFI_EVENT_SCAN_DONE: {
        const wifi_event_sta_scan_done_t *d = (const wifi_event_sta_scan_done_t *)data;
        s_scan_state = (d->status == 0) ? k_scan_done : k_scan_failed;
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        s_has_ip = true;
        s_retry_once = false;
        post(ev_got_ip, (int32_t)((ip_event_got_ip_t *)data)->ip_info.ip.addr);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_has_ip = false;
        post(ev_lost_ip);
    }
}

static void log_err(const char *what, esp_err_t err) {
    serial_log("[wifi] %s failed: %s", what, esp_err_to_name(err));
}

void wifi_net_init(void) {
    if (s_netif) {
        return;
    }
    s_events = xQueueCreate(16, sizeof(NetEvent));

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        log_err("esp_netif_init", err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* INVALID_STATE: already created */
        log_err("event loop", err);
    }
    s_netif = esp_netif_create_default_wifi_sta();

    /* Hostname as Arduino named it: esp32s3-XXXXXX (last 3 bytes of the MAC). */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char host[24];
    snprintf(host, sizeof(host), "esp32s3-%02X%02X%02X", mac[3], mac[4], mac[5]);
    esp_netif_set_hostname(s_netif, host);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        log_err("esp_wifi_init", err);
        return;
    }
    /* Credentials live on SD (wifi_store): never copy them into internal flash. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip_event, nullptr, nullptr);

    /* Radio stays on in STA mode from here; reconnects only drop the association. */
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        log_err("esp_wifi_start", err);
        return;
    }
    /* No modem power-save: dozing between beacons can miss the AP's auth replies (AUTH_EXPIRE,
     * slow joins). USB powered with the screen on, so the extra current does not matter. */
    esp_wifi_set_ps(WIFI_PS_NONE);
}

void wifi_net_poll(void) {
    if (!s_events) {
        return;
    }
    NetEvent e;
    while (xQueueReceive(s_events, &e, 0) == pdTRUE) {
        switch (e.id) {
        case ev_start:
            serial_log("[wifi] STA started");
            break;
        case ev_stop:
            serial_log("[wifi] STA stopped");
            break;
        case ev_connected:
            serial_log("[wifi] associated with AP (channel %d, RSSI %d dBm)", (int)e.a, wifi_net_rssi());
            break;
        case ev_disconnected:
            serial_log("[wifi] disconnected, reason %d (%s), RSSI %d dBm", (int)e.a, reason_name((int)e.a), (int)e.b);
            break;
        case ev_reconnect:
            serial_log("[wifi] reconnecting (after %s)", reason_name((int)e.a));
            break;
        case ev_got_ip: {
            const uint32_t a = (uint32_t)e.a; /* network byte order: first octet in the low byte */
            serial_log("[wifi] got IP %u.%u.%u.%u", (unsigned)(a & 0xFF), (unsigned)((a >> 8) & 0xFF),
                       (unsigned)((a >> 16) & 0xFF), (unsigned)(a >> 24));
            break;
        }
        case ev_lost_ip:
            serial_log("[wifi] lost IP");
            break;
        default:
            break;
        }
    }
}

bool wifi_net_connected(void) {
    return s_has_ip;
}

void wifi_net_ssid(char *out, size_t n) {
    if (!n) {
        return;
    }
    out[0] = '\0';
    wifi_ap_record_t ap;
    if (s_has_ip && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(out, n, "%s", (const char *)ap.ssid);
    }
}

void wifi_net_ip(char *out, size_t n) {
    if (!n) {
        return;
    }
    out[0] = '\0';
    esp_netif_ip_info_t ip;
    if (s_has_ip && s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(out, n, IPSTR, IP2STR(&ip.ip));
    }
}

int wifi_net_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

bool wifi_net_scan_start(bool show_hidden, uint32_t max_ms_per_chan) {
    if (s_scan_state == k_scan_running) {
        return true;
    }
    s_scan.clear();
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = show_hidden;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = 100;
    cfg.scan_time.active.max = max_ms_per_chan;
    s_scan_state = k_scan_running;
    const esp_err_t err = esp_wifi_scan_start(&cfg, false /* async: SCAN_DONE event */);
    if (err != ESP_OK) {
        log_err("scan start", err);
        s_scan_state = k_scan_failed;
        return false;
    }
    return true;
}

int wifi_net_scan_poll(void) {
    switch (s_scan_state) {
    case k_scan_running:
        return -1;
    case k_scan_failed:
        return -2;
    case k_scan_done: {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        s_scan.resize(n);
        if (n) {
            /* Copies and frees the driver's list; n becomes the number actually copied. */
            esp_wifi_scan_get_ap_records(&n, s_scan.data());
            s_scan.resize(n);
        }
        s_scan_state = k_scan_idle; /* results stay in s_scan until the next scan */
        return (int)n;
    }
    default:
        return (int)s_scan.size();
    }
}

const wifi_ap_record_t *wifi_net_scan_results(int *count) {
    if (count) {
        *count = (int)s_scan.size();
    }
    return s_scan.data();
}

bool wifi_net_connect(const char *ssid, const char *password, int channel, const uint8_t *bssid) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (password && strlen(password) > 64)) {
        serial_log("[wifi] connect: bad SSID or password length");
        return false;
    }
    wifi_config_t conf = {};
    snprintf((char *)conf.sta.ssid, sizeof(conf.sta.ssid), "%s", ssid);
    conf.sta.channel = (uint8_t)(channel > 0 ? channel : 0);
    conf.sta.scan_method = WIFI_FAST_SCAN;
    conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    conf.sta.threshold.rssi = -127;
    conf.sta.pmf_cfg.capable = true;
    if (password && password[0]) {
        memcpy(conf.sta.password, password, strlen(password)); /* 64 bytes: may be a raw PSK */
        conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    if (bssid) {
        conf.sta.bssid_set = true;
        memcpy(conf.sta.bssid, bssid, 6);
    }
    s_auto_reconnect = false; /* the disconnect below must not trigger a reconnect */
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &conf);
    if (err != ESP_OK) {
        log_err("set config", err);
        return false;
    }
    s_auto_reconnect = true;
    s_retry_once = true;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        log_err("connect", err);
        return false;
    }
    return true;
}

void wifi_net_disconnect(void) {
    s_auto_reconnect = false;
    s_retry_once = false;
    esp_wifi_disconnect();
}
