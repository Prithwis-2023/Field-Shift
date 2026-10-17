/**
 * Rocket flight computer - step 1: audio alert test (no sensors, no network)
 * Plays a distinct tone pattern for each simulated flight event.
 */
#include "tal_api.h"
#include "tkl_output.h"
#include "tdl_audio_manage.h"
#include "board_com_api.h"

#define SAMPLE_RATE   16000   /* assumption: 16 kHz, 16-bit, mono PCM */
#define MAX_TONE_MS   300
#define VOLUME        80

typedef enum {
    FS_ARMED, FS_LIFTOFF, FS_APOGEE, FS_MAIN, FS_LANDED, FS_COUNT
} flight_state_t;

static const char *sg_state_name[FS_COUNT] = {
    "ARMED", "LIFTOFF", "APOGEE", "MAIN DEPLOYED", "LANDED"
};

static TDL_AUDIO_HANDLE_T sg_audio = NULL;

/* 32-point sine table, amplitude ~16000 (half scale to avoid clipping) */
static const int16_t sg_sine[32] = {
    0, 3196, 6270, 9102, 11585, 13623, 15137, 16069,
    16384, 16069, 15137, 13623, 11585, 9102, 6270, 3196,
    0, -3196, -6270, -9102, -11585, -13623, -15137, -16069,
    -16384, -16069, -15137, -13623, -11585, -9102, -6270, -3196
};

static void __mic_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status,
                     uint8_t *data, uint32_t len)
{
    (void)type; (void)status; (void)data; (void)len;   /* mic unused */
}

/* Play one sine tone. freq_hz == 0 means silence. */
static void __tone(uint32_t freq_hz, uint32_t ms)
{
    if (ms > MAX_TONE_MS) ms = MAX_TONE_MS;

    if (freq_hz == 0) {
        tal_system_sleep(ms);
        return;
    }

    uint32_t samples = (SAMPLE_RATE * ms) / 1000;
    uint32_t bytes = samples * sizeof(int16_t);
    int16_t *buf = (int16_t *)tal_malloc(bytes);
    if (!buf) {
        PR_ERR("tone malloc failed");
        return;
    }

    uint32_t phase = 0;
    uint32_t step = (uint32_t)(((uint64_t)freq_hz * 32ULL * 65536ULL) / SAMPLE_RATE);
    uint32_t ramp = SAMPLE_RATE / 200;           /* 5 ms fade in/out, avoids clicks */
    if (ramp * 2 > samples) ramp = samples / 2;

    for (uint32_t i = 0; i < samples; i++) {
        int32_t s = sg_sine[(phase >> 16) & 31];
        phase += step;
        if (i < ramp)                s = s * (int32_t)i / (int32_t)ramp;
        else if (i > samples - ramp) s = s * (int32_t)(samples - i) / (int32_t)ramp;
        buf[i] = (int16_t)s;
    }

    tdl_audio_play(sg_audio, (uint8_t *)buf, bytes);
    tal_system_sleep(ms + 20);                   /* wait for playback to finish */
    tal_free(buf);
}

static void __play_event(flight_state_t st)
{
    switch (st) {
    case FS_ARMED:      /* one low tone */
        __tone(600, 300);
        break;
    case FS_LIFTOFF:    /* rising triple */
        __tone(800, 120);  __tone(1000, 120); __tone(1200, 120);
        break;
    case FS_APOGEE:     /* double high beep */
        __tone(1500, 150); __tone(0, 80); __tone(1500, 150);
        break;
    case FS_MAIN:       /* three quick beeps */
        __tone(1200, 100); __tone(0, 60);
        __tone(1200, 100); __tone(0, 60);
        __tone(1200, 100);
        break;
    case FS_LANDED:     /* locator chirp, repeats every 2 s */
        for (int i = 0; i < 5; i++) {
            __tone(2000, 80); __tone(2600, 80);
            tal_system_sleep(2000);
        }
        break;
    default:
        break;
    }
}

void user_main(void)
{
    OPERATE_RET rt = OPRT_OK;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("Rocket FC audio test | board: %s", PLATFORM_BOARD);

    board_register_hardware();

    rt = tdl_audio_find(AUDIO_CODEC_NAME, &sg_audio);
    if (rt != OPRT_OK) { PR_ERR("audio find failed: %d", rt); return; }

    rt = tdl_audio_open(sg_audio, __mic_cb);
    if (rt != OPRT_OK) { PR_ERR("audio open failed: %d", rt); return; }

    TDL_AUDIO_INFO_T info = {0};
    if (tdl_audio_get_info(sg_audio, &info) == OPRT_OK) {
        PR_NOTICE("audio info: rate=%d ch=%d bits=%d frame=%d",
                  info.sample_rate, info.sample_ch_num,
                  info.sample_bits, info.frame_size);
    }

    tdl_audio_volume_set(sg_audio, VOLUME);

    while (1) {
        PR_NOTICE("=== simulated flight starting in 5 s ===");
        tal_system_sleep(5000);

        for (int st = 0; st < FS_COUNT; st++) {
            PR_NOTICE("[%u ms] STATE -> %s",
                      (unsigned)tal_system_get_millisecond(), sg_state_name[st]);
            __play_event((flight_state_t)st);
            tal_system_sleep(3000);              /* pause between events */
        }
    }
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[]) { user_main(); }
#else
static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    user_main();
    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = 1024 * 4;
    thrd_param.priority = THREAD_PRIO_1;
    thrd_param.thrdname = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif