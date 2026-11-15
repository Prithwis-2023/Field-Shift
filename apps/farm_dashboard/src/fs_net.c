#include "tuya_cloud_types.h"
#include "http_client_interface.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "netmgr.h"
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
#include "netconn_wired.h"
#endif
#include "tuya_register_center.h"

#include "fs_net.h"
#include "fs_core.h"
#include "fs_insight.h"
#include "fs_secrets.h"

/* certificate lookup: use the SDK header if it is on the include path, else declare it */
#if defined(__has_include)
#  if __has_include("iotdns.h")
#    include "iotdns.h"
#    define FS_HAVE_IOTDNS_H 1
#  endif
#endif
#ifndef FS_HAVE_IOTDNS_H
extern int tuya_iotdns_query_domain_certs(char *url, uint8_t **cacert, uint16_t *cacert_len);
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NASA_HOST        "power.larc.nasa.gov"
#define HTTP_TIMEOUT_MS  (15 * 1000)
#define OK_INTERVAL_MS   (3u * 3600u * 1000u)
#define RETRY_MIN_MS     (30u * 1000u)
#define RETRY_MAX_MS     (15u * 60u * 1000u)
#define WINDOW_DAYS      14

static volatile bool sg_link_up = false;

/* ------------------------------------------------------------ date helpers */
static int32_t days_from_civil(int32_t y, int32_t m, int32_t d)
{
    y -= (m <= 2);
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    int32_t yoe = y - era * 400;
    int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int32_t z, int32_t *y, int32_t *m, int32_t *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    int32_t doe = z - era * 146097;
    int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int32_t mp  = (5 * doy + 2) / 153;
    int32_t mm  = mp + (mp < 10 ? 3 : -9);
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mm;
    *y = yoe + era * 400 + (mm <= 2);
}

static bool parse_ymd(const char *s, int32_t *y, int32_t *m, int32_t *d)
{
    if (!s || strlen(s) != 8) return false;
    for (int i = 0; i < 8; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    *y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    *m = (s[4] - '0') * 10 + (s[5] - '0');
    *d = (s[6] - '0') * 10 + (s[7] - '0');
    return (*m >= 1 && *m <= 12 && *d >= 1 && *d <= 31);
}

/* window ends yesterday (relative to the configured date plus uptime days) */
static bool make_window(char *start, char *end)
{
    int32_t y, m, d;
    if (!parse_ymd(FS_START_DATE_YMD, &y, &m, &d)) return false;

    int32_t up_days = (int32_t)((uint32_t)tal_system_get_millisecond() / 86400000u);
    int32_t end_days = days_from_civil(y, m, d) + up_days - 1;
    int32_t start_days = end_days - (WINDOW_DAYS - 1);

    civil_from_days(start_days, &y, &m, &d);
    snprintf(start, 9, "%04d%02d%02d", (int)y, (int)m, (int)d);
    civil_from_days(end_days, &y, &m, &d);
    snprintf(end, 9, "%04d%02d%02d", (int)y, (int)m, (int)d);
    return true;
}

/* --------------------------------------------------------------- JSON scan */
/* newest valid value of "<param>":{"YYYYMMDD":v,...} inside the "parameter" block.
   The fill value (-999) is skipped. No allocation. */
static bool latest_valid(const char *json, const char *param, float *val, char *date_out)
{
    const char *blk = strstr(json, "\"parameter\"");
    if (!blk) return false;

    char key[32];
    snprintf(key, sizeof(key), "\"%s\"", param);
    const char *p = strstr(blk, key);
    if (!p) return false;
    p = strchr(p, '{');
    if (!p) return false;
    p++;

    bool found = false;
    while (*p && *p != '}') {
        const char *q = strchr(p, '"');
        if (!q) break;
        q++;
        const char *qe = strchr(q, '"');
        if (!qe || (qe - q) != 8) break;      /* not a date key: stop */

        char day[9];
        memcpy(day, q, 8);
        day[8] = '\0';

        p = qe + 1;
        while (*p == ':' || *p == ' ') p++;
        char *endp = NULL;
        float v = strtof(p, &endp);
        if (endp == p) break;
        p = endp;

        if (v > -990.0f) {
            *val = v;
            memcpy(date_out, day, 9);
            found = true;
        }
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    }
    return found;
}

/* ------------------------------------------------------------------- fetch */
#define FS_MAX_BODY 16384

static int fetch_once(void)
{
    char d0[9], d1[9], path[300];
    if (!make_window(d0, d1)) {
        PR_ERR("FS_START_DATE_YMD in fs_secrets.h is not YYYYMMDD");
        return -1;
    }
    snprintf(path, sizeof(path),
             "/api/temporal/daily/point?parameters=GWETTOP,PRECTOTCORR&community=AG"
             "&longitude=%s&latitude=%s&start=%s&end=%s&format=JSON",
             FS_SITE_LON, FS_SITE_LAT, d0, d1);
    PR_NOTICE("POWER request: %s", path);

    char host[] = NASA_HOST;
    uint16_t cacert_len = 0;
    uint8_t *cacert = NULL;
    OPERATE_RET rt = tuya_iotdns_query_domain_certs(host, &cacert, &cacert_len);
    if (rt != OPRT_OK) {
        PR_ERR("cert query for %s failed: %d", NASA_HOST, rt);
        return -1;
    }

    http_client_response_t resp = {0};
    http_client_header_t headers[] = {{.key = "Accept", .value = "application/json"}};

    http_client_status_t hs = http_client_request(
        &(const http_client_request_t){.cacert = cacert,
                                       .cacert_len = cacert_len,
                                       .host = NASA_HOST,
                                       .port = 443,
                                       .method = "GET",
                                       .path = path,
                                       .headers = headers,
                                       .headers_count = sizeof(headers) / sizeof(http_client_header_t),
                                       .body = (const uint8_t *)"",
                                       .body_length = 0,
                                       .timeout_ms = HTTP_TIMEOUT_MS},
        &resp);

    int result = -1;
    char *body = NULL;

    if (hs != HTTP_CLIENT_SUCCESS) {
        PR_ERR("http request failed: %d (%s)", (int)hs,
               (hs == HTTP_CLIENT_SERIALIZE_FAULT) ? "serialize" :
               (hs == HTTP_CLIENT_SEND_FAULT) ? "send / TLS / network" :
               (hs == HTTP_CLIENT_MALLOC_FAULT) ? "out of memory" : "unknown");
    } else if (!resp.body || resp.body_length == 0 || resp.body_length > FS_MAX_BODY) {
        PR_ERR("bad response body (length %d, HTTP status %d)", (int)resp.body_length, (int)resp.status_code);
    } else {
        /* copy with a terminator: the SDK buffer is not guaranteed to be a C string */
        body = (char *)tal_malloc(resp.body_length + 1);
        if (!body) {
            PR_ERR("no memory for reply copy (%d bytes)", (int)resp.body_length + 1);
        } else {
            memcpy(body, resp.body, resp.body_length);
            body[resp.body_length] = '\0';
            PR_NOTICE("POWER reply HTTP %d, %d bytes, free heap %d", (int)resp.status_code, (int)resp.body_length,
                      tal_system_get_free_heap_size());

            float w = 0.0f, r = 0.0f;
            char dw[9] = {0}, dr[9] = {0};
            bool ok_w = latest_valid(body, "GWETTOP", &w, dw);
            bool ok_r = latest_valid(body, "PRECTOTCORR", &r, dr);

            if (!ok_w && !ok_r) {
                char prev[121];
                size_t n = (resp.body_length < 120) ? resp.body_length : 120;
                memcpy(prev, body, n);
                prev[n] = '\0';
                PR_ERR("no usable values in reply. Start of reply: %s", prev);
            } else {
                if (ok_w) fs_publish(fs_find("macro"), w * 100.0f);
                if (ok_r) fs_publish(fs_find("rain"), r);
                if (ok_w) {
                    char src[64];
                    snprintf(src, sizeof(src), "NASA POWER/MERRA-2 model, %c%c%c%c-%c%c-%c%c",
                             dw[0], dw[1], dw[2], dw[3], dw[4], dw[5], dw[6], dw[7]);
                    ins_set_source(src);
                }
                int wp = (int)(w * 100.0f + 0.5f);
                int r10 = (int)(r * 10.0f + 0.5f);
                PR_NOTICE("POWER ok: soil wetness %d%% (%s), rain %d.%d mm/day (%s)",
                          ok_w ? wp : -1, ok_w ? dw : "none",
                          ok_r ? r10 / 10 : -1, ok_r ? r10 % 10 : 0, ok_r ? dr : "none");
                result = 0;
            }
        }
    }

    if (body) tal_free(body);
    http_client_free(&resp);
    return result;
}

/* ----------------------------------------------------------- network setup */
static OPERATE_RET link_status_cb(void *data)
{
    netmgr_status_e st = *((netmgr_status_e *)data);   /* form used by the https_client example */
    sg_link_up = (st == NETMGR_LINK_UP || st == NETMGR_LINK_UP_SWITH);
    PR_NOTICE("net link %s", sg_link_up ? "UP" : "DOWN");
    return OPRT_OK;
}

static void net_init(void)
{
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();
    tuya_tls_init();
    tuya_register_center_init();
    tal_event_subscribe(EVENT_LINK_STATUS_CHG, "fs_net", link_status_cb, SUBSCRIBE_TYPE_NORMAL);

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    netmgr_type_e type = 0;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    type |= NETCONN_WIFI;
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
    type |= NETCONN_WIRED;
#endif
    netmgr_init(type);

#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    netconn_wifi_info_t wifi_info = {0};
    strncpy(wifi_info.ssid, FS_WIFI_SSID, sizeof(wifi_info.ssid) - 1);
    strncpy(wifi_info.pswd, FS_WIFI_PSWD, sizeof(wifi_info.pswd) - 1);
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &wifi_info);
#endif
}

static void net_task(void *arg)
{
    (void)arg;

    if (strcmp(FS_WIFI_SSID, "your-ssid") == 0) {
        PR_ERR("fs_secrets.h not filled in: network task idle");
        while (1) tal_system_sleep(60000);
    }

    net_init();

    uint32_t backoff = RETRY_MIN_MS;
    bool seen_up = false;
    while (1) {
        if (!sg_link_up) {
            tal_system_sleep(1000);
            continue;
        }
        if (!seen_up) {            /* give DHCP a moment before the first request */
            seen_up = true;
            tal_system_sleep(2000);
        }
        if (fetch_once() == 0) {
            backoff = RETRY_MIN_MS;
            tal_system_sleep(OK_INTERVAL_MS);
        } else {
            PR_ERR("fetch failed, retry in %u s", (unsigned)(backoff / 1000u));
            tal_system_sleep(backoff);
            backoff = (backoff * 2u > RETRY_MAX_MS) ? RETRY_MAX_MS : backoff * 2u;
        }
    }
}

void fs_net_start(void)
{
    THREAD_HANDLE h = NULL;
    THREAD_CFG_T cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.stackDepth = 1024 * 12;           /* TLS handshake needs a big stack */
    cfg.priority   = THREAD_PRIO_2;
    cfg.thrdname   = "fs_net";
    if (tal_thread_create_and_start(&h, NULL, NULL, net_task, NULL, &cfg) != OPRT_OK) {
        PR_ERR("fs_net thread create failed");
    }
}
