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
#include "fs_time.h"
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
#define FS_MAX_BODY      16384
#define SER_MAX          16

static volatile bool sg_link_up = false;
static volatile bool sg_refetch = false;

/* ---- feed table: one row per NASA POWER value that becomes a channel ----
   Adding a feed = one row here + one row in the sensor table of the profile.
   sum_days > 0 means: sum of the newest sum_days valid days instead of the newest value. */
typedef struct {
    const char *param;
    const char *key;
    float       mult;
    int         sum_days;
} feed_t;

static const feed_t FEEDS[] = {
    { "GWETTOP",     "macro", 100.0f, 0 },    /* surface soil wetness 0..1 -> %  */
    { "GWETROOT",    "root",  100.0f, 0 },    /* root-zone wetness 0..1 -> %     */
    { "PRECTOTCORR", "rain",    1.0f, 0 },    /* newest valid day, mm/day        */
    { "PRECTOTCORR", "rain7",   1.0f, 7 },    /* last 7 valid days, mm           */
    { "T2M",         "rt2m",    1.0f, 0 },    /* air temperature, C              */
    { "T2M_MIN",     "rtmin",   1.0f, 0 },    /* daily minimum temperature, C    */
    { "RH2M",        "rh",      1.0f, 0 },    /* relative humidity, %            */
};
#define N_FEEDS ((int)(sizeof(FEEDS) / sizeof(FEEDS[0])))

/* ------------------------------------------------------------ small helpers */
static void fmt2(char *b, size_t n, float v)
{
    int neg = (v < 0.0f);
    if (neg) v = -v;
    int32_t c = (int32_t)(v * 100.0f + 0.5f);
    snprintf(b, n, "%s%d.%02d", neg ? "-" : "", (int)(c / 100), (int)(c % 100));
}

static bool has_token(const char *list, const char *tok)
{
    size_t tl = strlen(tok);
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == tl && strncmp(p, tok, tl) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static void build_params(char *out, size_t n)
{
    out[0] = '\0';
    for (int i = 0; i < N_FEEDS; i++) {
        if (fs_find(FEEDS[i].key) < 0) continue;          /* no channel, no request */
        if (has_token(out, FEEDS[i].param)) continue;
        if (out[0]) strncat(out, ",", n - strlen(out) - 1);
        strncat(out, FEEDS[i].param, n - strlen(out) - 1);
    }
}

/* ------------------------------------------------------------- HTTP Date */
/* header block is not a C string: scan by length. name must be lower case. */
static bool header_value(const uint8_t *h, size_t n, const char *name, char *out, size_t outsz)
{
    size_t nl = strlen(name);
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && h[j] != '\n') j++;
        size_t len = j - i;
        if (len > nl + 1 && h[i + nl] == ':') {
            bool match = true;
            for (size_t k = 0; k < nl; k++) {
                char c = (char)h[i + k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (c != name[k]) { match = false; break; }
            }
            if (match) {
                size_t v = i + nl + 1;
                while (v < j && (h[v] == ' ' || h[v] == '\t')) v++;
                size_t e = j;
                while (e > v && (h[e - 1] == '\r' || h[e - 1] == ' ')) e--;
                size_t cnt = e - v;
                if (cnt >= outsz) cnt = outsz - 1;
                memcpy(out, h + v, cnt);
                out[cnt] = '\0';
                return true;
            }
        }
        i = j + 1;
    }
    return false;
}

/* "Sat, 14 Nov 2026 10:22:33 GMT" */
static bool parse_http_date(const char *s, uint32_t *epoch)
{
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *p = strchr(s, ',');
    p = p ? p + 1 : s;
    while (*p == ' ') p++;

    char *e = NULL;
    long day = strtol(p, &e, 10);
    if (e == p) return false;
    p = e;
    while (*p == ' ') p++;

    int m = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(p, mon + 3 * i, 3) == 0) { m = i + 1; break; }
    }
    if (!m) return false;
    p += 3;

    long year = strtol(p, &e, 10);
    if (e == p) return false;
    p = e;
    long hh = strtol(p, &e, 10);
    if (e == p || *e != ':') return false;
    p = e + 1;
    long mi = strtol(p, &e, 10);
    if (e == p || *e != ':') return false;
    p = e + 1;
    long ss = strtol(p, &e, 10);
    if (e == p) return false;

    if (year < 2020 || year > 2100 || day < 1 || day > 31 || hh > 23 || mi > 59 || ss > 60) return false;

    int32_t days = fs_days_from_civil((int32_t)year, (int32_t)m, (int32_t)day);
    *epoch = (uint32_t)days * 86400u + (uint32_t)hh * 3600u + (uint32_t)mi * 60u + (uint32_t)ss;
    return true;
}

/* window ends yesterday: synced clock if we have it, else FS_START_DATE_YMD + uptime */
static bool make_window(char *start, char *end, int32_t *end_days_out, bool *fallback_out)
{
    int32_t end_days;
    if (fs_time_valid()) {
        end_days = (int32_t)(fs_time_epoch() / 86400u) - 1;
        *fallback_out = false;
    } else {
        const char *s = FS_START_DATE_YMD;
        if (strlen(s) != 8) return false;
        for (int i = 0; i < 8; i++) {
            if (s[i] < '0' || s[i] > '9') return false;
        }
        int32_t y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
        int32_t m = (s[4] - '0') * 10 + (s[5] - '0');
        int32_t d = (s[6] - '0') * 10 + (s[7] - '0');
        if (m < 1 || m > 12 || d < 1 || d > 31) return false;
        int32_t up_days = (int32_t)((uint32_t)tal_system_get_millisecond() / 86400000u);
        end_days = fs_days_from_civil(y, m, d) + up_days - 1;
        *fallback_out = true;
    }
    int32_t y, m, d;
    fs_civil_from_days(end_days - (WINDOW_DAYS - 1), &y, &m, &d);
    snprintf(start, 9, "%04d%02d%02d", (int)y, (int)m, (int)d);
    fs_civil_from_days(end_days, &y, &m, &d);
    snprintf(end, 9, "%04d%02d%02d", (int)y, (int)m, (int)d);
    *end_days_out = end_days;
    return true;
}

/* --------------------------------------------------------------- JSON scan */
typedef struct {
    int   n;
    float v[SER_MAX];
    char  last_date[9];
} series_t;

/* valid values of "<param>":{"YYYYMMDD":v,...} inside the "parameter" block, oldest first.
   The fill value (-999) is skipped. No allocation. */
static bool parse_series(const char *json, const char *param, series_t *out)
{
    out->n = 0;
    out->last_date[0] = '\0';

    const char *blk = strstr(json, "\"parameter\"");
    if (!blk) return false;

    char key[32];
    snprintf(key, sizeof(key), "\"%s\"", param);
    const char *p = strstr(blk, key);
    if (!p) return false;
    p = strchr(p, '{');
    if (!p) return false;
    p++;

    while (*p && *p != '}') {
        const char *q = strchr(p, '"');
        if (!q) break;
        q++;
        const char *qe = strchr(q, '"');
        if (!qe || (qe - q) != 8) break;       /* not a date key: stop */

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
            if (out->n == SER_MAX) {
                memmove(out->v, out->v + 1, (SER_MAX - 1) * sizeof(float));
                out->n--;
            }
            out->v[out->n++] = v;
            memcpy(out->last_date, day, 9);
        }
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    }
    return out->n > 0;
}

static int publish_feeds(const char *body, char *macro_date)
{
    int published = 0;
    for (int i = 0; i < N_FEEDS; i++) {
        const feed_t *f = &FEEDS[i];
        int ch = fs_find(f->key);
        if (ch < 0) continue;

        series_t s;
        if (!parse_series(body, f->param, &s)) {
            PR_NOTICE("POWER: no valid data for %s", f->param);
            continue;
        }
        float val = 0.0f;
        if (f->sum_days > 0) {
            int n = (s.n < f->sum_days) ? s.n : f->sum_days;
            for (int k = s.n - n; k < s.n; k++) val += s.v[k];
        } else {
            val = s.v[s.n - 1];
        }
        val *= f->mult;
        fs_publish(ch, val);

        char b[20];
        fmt2(b, sizeof(b), val);
        PR_NOTICE("POWER %s -> %s = %s (newest valid day %s)", f->param, f->key, b, s.last_date);
        if (strcmp(f->key, "macro") == 0) memcpy(macro_date, s.last_date, 9);
        published++;
    }
    return published;
}

/* ------------------------------------------------------------------- fetch */
static int fetch_once(void)
{
    char d0[9], d1[9], params[96], path[360];
    int32_t end_days = 0;
    bool used_fallback = false;

    if (!make_window(d0, d1, &end_days, &used_fallback)) {
        PR_ERR("FS_START_DATE_YMD in fs_secrets.h is not YYYYMMDD");
        return -1;
    }
    build_params(params, sizeof(params));
    snprintf(path, sizeof(path),
             "/api/temporal/daily/point?parameters=%s&community=AG"
             "&longitude=%s&latitude=%s&start=%s&end=%s&format=JSON",
             params, FS_SITE_LON, FS_SITE_LAT, d0, d1);
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
    } else {
        /* clock: the Date header of any reply is the current UTC time */
        char dv[48];
        if (resp.headers && resp.headers_length > 0 &&
            header_value(resp.headers, resp.headers_length, "date", dv, sizeof(dv))) {
            uint32_t epoch = 0;
            if (parse_http_date(dv, &epoch)) {
                fs_time_set_epoch(epoch);
                char ymd[9];
                int hh = 0, mm = 0;
                fs_time_ymd(ymd);
                (void)fs_time_hm(&hh, &mm);
                PR_NOTICE("clock synced: %s %02d:%02d UTC", ymd, hh, mm);
                if (used_fallback && ((int32_t)(fs_time_epoch() / 86400u) - 1) != end_days) {
                    sg_refetch = true;      /* the fallback date was off: ask again with the real one */
                }
            } else {
                PR_ERR("could not parse Date header: %s", dv);
            }
        } else {
            PR_NOTICE("no Date header in reply: clock not synced");
        }

        if (!resp.body || resp.body_length == 0 || resp.body_length > FS_MAX_BODY) {
            PR_ERR("bad response body (length %d, HTTP status %d)", (int)resp.body_length, (int)resp.status_code);
        } else {
            body = (char *)tal_malloc(resp.body_length + 1);
            if (!body) {
                PR_ERR("no memory for reply copy (%d bytes)", (int)resp.body_length + 1);
            } else {
                memcpy(body, resp.body, resp.body_length);
                body[resp.body_length] = '\0';
                PR_NOTICE("POWER reply HTTP %d, %d bytes, free heap %d", (int)resp.status_code,
                          (int)resp.body_length, tal_system_get_free_heap_size());

                char macro_date[9] = {0};
                int n = publish_feeds(body, macro_date);
                if (n == 0) {
                    char prev[121];
                    size_t k = (resp.body_length < 120) ? resp.body_length : 120;
                    memcpy(prev, body, k);
                    prev[k] = '\0';
                    PR_ERR("no usable values in reply. Start of reply: %s", prev);
                } else {
                    if (macro_date[0]) {
                        char src[64];
                        snprintf(src, sizeof(src), "NASA POWER/MERRA-2 model, %c%c%c%c-%c%c-%c%c",
                                 macro_date[0], macro_date[1], macro_date[2], macro_date[3],
                                 macro_date[4], macro_date[5], macro_date[6], macro_date[7]);
                        ins_set_source(src);
                    }
                    PR_NOTICE("POWER ok: %d channels updated", n);
                    result = 0;
                }
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
            if (sg_refetch) {
                sg_refetch = false;
                PR_NOTICE("first window used the fallback date: refetching with the synced clock");
                tal_system_sleep(1000);
                continue;
            }
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
