/*
 * USB Audio Exclusive Output Engine
 * Sends raw PCM/DSD data directly to USB DAC via isochronous transfers.
 * Bypasses Android's audio mixer for bit-perfect output.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 三水深
 * Uses libusb (LGPL-2.1) for USB access; see THIRD_PARTY_NOTICES.md.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <libusb.h>
#include <jni.h>
#include <android/log.h>

#define LOG_TAG "UsbAudioOutput"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* USB Audio Class constants */
#define USB_CLASS_AUDIO             0x01
#define USB_SUBCLASS_AUDIOCONTROL   0x01
#define USB_SUBCLASS_AUDIOSTREAMING 0x02

/* USB Audio Class specific request codes */
#define UAC_SET_CUR     0x01
#define UAC_GET_CUR     0x81
#define UAC_SET_MIN     0x02
#define UAC_GET_MIN     0x82
#define UAC_SET_MAX     0x03
#define UAC_GET_MAX     0x83
#define UAC_SET_RES     0x04
#define UAC_GET_RES     0x84
#define UAC_GET_RANGE   0x84  /* UAC2 */

/* Audio Control selector codes */
#define UAC_FU_MUTE     0x01
#define UAC_FU_VOLUME   0x02

/* UAC1 Endpoint Control Selectors */
#define UAC_EP_CS_ATTR_SAMPLE_RATE   0x01
#define UAC_EP_CS_ATTR_PITCH         0x02

/* UAC2 Clock Source Control Selectors */
#define UAC2_CS_CONTROL_SAM_FREQ     0x01
#define UAC2_CS_CONTROL_CLOCK_VALID  0x02

/* Audio Data Format Type I codes (UAC2) */
#define UAC2_FORMAT_TYPE_I_PCM       (1 << 0)
#define UAC2_FORMAT_TYPE_I_PCM8      (1 << 1)
#define UAC2_FORMAT_TYPE_I_IEEE_FLOAT (1 << 2)
#define UAC2_FORMAT_TYPE_I_ALAW      (1 << 3)
#define UAC2_FORMAT_TYPE_I_MULAW     (1 << 4)
#define UAC2_FORMAT_TYPE_I_RAW_DSD   (1 << 5)  /* DSD (native) */

/* Transfer configuration */
#define NUM_OUT_TRANSFERS    16   /* more in-flight transfers absorb scheduling jitter */
#define MAX_PACKET_SIZE      1024
#define ISO_PACKETS_PER_XFR  24
#define NUM_FB_TRANSFERS     4    /* asynchronous feedback IN transfers */
#define RING_BUFFER_SIZE     (1024 * 1024 * 2)  /* 2MB ring buffer */
#define FEED_TIMEOUT_MS      100

/* Module state */
static struct {
    libusb_context *ctx;
    libusb_device_handle *devh;
    JavaVM *java_vm;

    /* Device info */
    int vendor_id;
    int product_id;
    char device_name[256];
    char manufacturer[256];

    /* Audio streaming interface */
    int iface_num;
    int alt_setting;
    uint8_t ep_out;          /* OUT endpoint address */
    int ep_max_packet_size;
    int ep_interval;         /* bInterval of the chosen OUT endpoint */
    int dev_speed;           /* libusb_speed of the device */

    /* Asynchronous feedback IN endpoint (tells us the device's real rate) */
    uint8_t ep_feedback;     /* feedback IN endpoint addr, 0 if none */
    int ep_feedback_mps;
    int ep_feedback_interval;
    struct libusb_transfer *fb_transfers[NUM_FB_TRANSFERS];
    uint8_t *fb_buffers[NUM_FB_TRANSFERS];
    volatile double fb_samples_per_pkt;  /* live target frames/packet, 0 = none */
    double fb_acc;                        /* fractional frame accumulator */

    /* Audio format */
    int sample_rate;
    int bit_depth;           /* 16, 24, 32 */
    int channels;
    int is_dsd;              /* 0 = PCM, 1 = DSD over PCM (DoP), 2 = native DSD */
    int is_uac2;             /* UAC version: 0=UAC1, 1=UAC2 */

    /* Isochronous packet sizing (computed in start_iso_output) */
    int bytes_per_frame;     /* (bit_depth/8) * channels */
    int packets_per_sec;     /* iso service intervals per second */
    int frames_per_packet;   /* base whole frames per service interval */
    int frames_remainder;    /* sample_rate % packets_per_sec */
    int frac_acc;            /* running accumulator for the fractional frame */
    int packet_stride;       /* per-packet allocation stride in the iso buffer */
    volatile int primed;     /* 0 until the ring has filled to prime_threshold */
    int prime_threshold;     /* bytes of cushion to build before real output */

    /* UAC2 specific */
    int clock_source_id;     /* Clock Source Entity ID for UAC2 */
    int feature_unit_id;     /* Feature Unit ID for volume control */
    int mixer_unit_id;       /* Mixer Unit ID */
    int ac_iface_num;        /* AudioControl interface hosting the entities */

    /* Transfer state */
    struct libusb_transfer *out_transfers[NUM_OUT_TRANSFERS];
    uint8_t *transfer_buffers[NUM_OUT_TRANSFERS];
    volatile int running;
    pthread_t event_thread;

    /* Ring buffer for PCM data from Java */
    uint8_t *ring_buffer;
    volatile int ring_read_pos;
    volatile int ring_write_pos;
    int ring_size;
    pthread_mutex_t ring_mutex;
    pthread_cond_t ring_cond;

    /* Statistics */
    unsigned long bytes_written;
    unsigned long underrun_count;
    struct timeval start_time;

    /* Callbacks */
    jclass callback_class;
    jmethodID on_error_method;
    jmethodID on_buffer_status_method;
    jobject callback_object;
} g_state;

/* ============================================================
 * Ring Buffer Operations
 * ============================================================ */

static int ring_buffer_available_read(void) {
    int available = g_state.ring_write_pos - g_state.ring_read_pos;
    if (available < 0) available += g_state.ring_size;
    return available;
}

static int ring_buffer_available_write(void) {
    int available = g_state.ring_size - ring_buffer_available_read() - 1;
    return available;
}

static int ring_buffer_read(uint8_t *dst, int len) {
    int available = ring_buffer_available_read();
    if (len > available) len = available;
    if (len <= 0) return 0;

    int read_pos = g_state.ring_read_pos;
    int first_part = g_state.ring_size - read_pos;
    if (first_part > len) first_part = len;

    memcpy(dst, g_state.ring_buffer + read_pos, first_part);
    if (first_part < len) {
        memcpy(dst + first_part, g_state.ring_buffer, len - first_part);
    }

    g_state.ring_read_pos = (read_pos + len) % g_state.ring_size;
    return len;
}

/* ============================================================
 * USB Audio Class Descriptor Parsing
 * ============================================================ */

/* One streamable alternate setting with its PCM format. */
struct as_candidate {
    int iface_num;
    int alt_setting;
    uint8_t ep_out;
    int max_packet;
    int interval;
    int bits;       /* bBitResolution, 0 if unknown */
    int subslot;    /* bytes per sample, 0 if unknown */
    int channels;   /* 0 if unknown */
    uint8_t ep_fb;  /* asynchronous feedback IN endpoint, 0 if none */
    int fb_mps;
    int fb_interval;
};

/* Parse USB Audio Class descriptors. Selects the AudioStreaming alternate
 * setting whose PCM format best matches the requested bit depth / channels. */
static int parse_uac_descriptors(int want_bits, int want_ch) {
    libusb_device *dev = libusb_get_device(g_state.devh);
    struct libusb_config_descriptor *config;
    int rc = libusb_get_active_config_descriptor(dev, &config);
    if (rc < 0) {
        LOGE("Failed to get config descriptor: %d", rc);
        return -1;
    }

    g_state.iface_num = -1;
    g_state.alt_setting = -1;
    g_state.ep_out = 0;
    g_state.ep_interval = 1;
    g_state.ep_feedback = 0;
    g_state.clock_source_id = -1;
    g_state.feature_unit_id = -1;
    g_state.ac_iface_num = 0;

    struct as_candidate cands[32];
    int num_cands = 0;

    LOGI("Config has %d interfaces", config->bNumInterfaces);

    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &config->interface[i];
        for (int j = 0; j < iface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[j];

            if (alt->bInterfaceClass != USB_CLASS_AUDIO) continue;
            if (alt->bInterfaceSubClass != USB_SUBCLASS_AUDIOSTREAMING) continue;
            if (alt->bAlternateSetting == 0) continue;  /* alt 0 = zero bandwidth */

            /* Find the isochronous OUT endpoint and (if async) its feedback IN. */
            uint8_t ep_addr = 0, fb_addr = 0;
            int ep_mps = 0, ep_intv = 1, fb_mps = 0, fb_intv = 1;
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];
                if ((ep->bmAttributes & 0x03) != 0x01) continue;        /* iso only */
                if ((ep->bEndpointAddress & 0x80) == 0) {               /* OUT (data) */
                    if (ep_addr == 0) {
                        ep_addr = ep->bEndpointAddress;
                        ep_mps = ep->wMaxPacketSize & 0x07FF;  /* bytes/packet */
                        ep_intv = ep->bInterval > 0 ? ep->bInterval : 1;
                    }
                } else {                                                /* IN (feedback) */
                    fb_addr = ep->bEndpointAddress;
                    fb_mps = ep->wMaxPacketSize & 0x07FF;
                    fb_intv = ep->bInterval > 0 ? ep->bInterval : 1;
                }
            }
            if (ep_addr == 0) continue;  /* no iso OUT in this alt */

            /* Parse this alt's class-specific descriptors for the PCM format. */
            int bits = 0, subslot = 0, chans = 0;
            const uint8_t *extra = alt->extra;
            int extra_len = alt->extra_length;
            while (extra_len >= 3) {
                uint8_t bLength = extra[0];
                uint8_t bType = extra[1];
                uint8_t bSubtype = extra[2];
                if (bLength < 3 || bLength > extra_len) break;

                if (bType == 0x24) {  /* CS_INTERFACE */
                    if (bSubtype == 0x01) {            /* AS_GENERAL */
                        /* UAC2: bNrChannels at offset 10. */
                        if (bLength >= 11) chans = extra[10];
                    } else if (bSubtype == 0x02) {     /* FORMAT_TYPE */
                        if (bLength >= 6 && extra[3] == 0x01) {  /* FORMAT_TYPE_I */
                            if (bLength >= 8) {
                                /* UAC1: [4]bNrChannels [5]bSubframeSize [6]bBitResolution */
                                if (chans == 0) chans = extra[4];
                                subslot = extra[5];
                                bits = extra[6];
                            } else {
                                /* UAC2: [4]bSubslotSize [5]bBitResolution */
                                subslot = extra[4];
                                bits = extra[5];
                            }
                        }
                    }
                }
                extra += bLength;
                extra_len -= bLength;
            }

            LOGI("AS iface %d alt %d: ep 0x%02X mps %d intv %d -> %dbit/%dch (subslot %d)",
                 alt->bInterfaceNumber, alt->bAlternateSetting, ep_addr,
                 ep_mps, ep_intv, bits, chans, subslot);

            if (num_cands < 32) {
                cands[num_cands].iface_num = alt->bInterfaceNumber;
                cands[num_cands].alt_setting = alt->bAlternateSetting;
                cands[num_cands].ep_out = ep_addr;
                cands[num_cands].max_packet = ep_mps;
                cands[num_cands].interval = ep_intv;
                cands[num_cands].bits = bits;
                cands[num_cands].subslot = subslot;
                cands[num_cands].channels = chans;
                cands[num_cands].ep_fb = fb_addr;
                cands[num_cands].fb_mps = fb_mps;
                cands[num_cands].fb_interval = fb_intv;
                num_cands++;
            }
        }
    }

    /* Choose the best candidate:
     *   1) exact bit depth + channel match
     *   2) exact bit depth match
     *   3) lowest alt setting (most basic format) */
    int best = -1;
    for (int c = 0; c < num_cands; c++) {
        if (cands[c].bits == want_bits && cands[c].channels == want_ch) { best = c; break; }
    }
    if (best < 0)
        for (int c = 0; c < num_cands; c++)
            if (cands[c].bits == want_bits) { best = c; break; }
    if (best < 0 && num_cands > 0) {
        best = 0;
        for (int c = 1; c < num_cands; c++)
            if (cands[c].alt_setting < cands[best].alt_setting) best = c;
    }

    if (best >= 0) {
        g_state.iface_num = cands[best].iface_num;
        g_state.alt_setting = cands[best].alt_setting;
        g_state.ep_out = cands[best].ep_out;
        g_state.ep_max_packet_size = cands[best].max_packet;
        g_state.ep_interval = cands[best].interval;
        g_state.ep_feedback = cands[best].ep_fb;
        g_state.ep_feedback_mps = cands[best].fb_mps;
        g_state.ep_feedback_interval = cands[best].fb_interval;
        if (cands[best].bits > 0) g_state.bit_depth = cands[best].bits;
        if (cands[best].channels > 0) g_state.channels = cands[best].channels;
        LOGI("Feedback EP: 0x%02X mps %d intv %d",
             g_state.ep_feedback, g_state.ep_feedback_mps, g_state.ep_feedback_interval);
    }

    /* Also scan AudioControl interface for Clock Source and Feature Unit */
    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &config->interface[i];
        for (int j = 0; j < iface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[j];
            if (alt->bInterfaceClass != USB_CLASS_AUDIO) continue;
            if (alt->bInterfaceSubClass != USB_SUBCLASS_AUDIOCONTROL) continue;

            /* Clock Source / Feature Unit entities live on THIS interface, so
             * class-specific requests must target this interface number. */
            g_state.ac_iface_num = alt->bInterfaceNumber;

            const uint8_t *extra = alt->extra;
            int extra_len = alt->extra_length;
            while (extra_len >= 3) {
                uint8_t bLength = extra[0];
                uint8_t bDescriptorType = extra[1];
                uint8_t bDescriptorSubtype = extra[2];

                if (bLength < 3 || bLength > extra_len) break;

                if (bDescriptorType == 0x24) {
                    /* UAC2: CLOCK_SOURCE (0x0A) */
                    if (bDescriptorSubtype == 0x0A && bLength >= 4) {
                        g_state.clock_source_id = extra[3];  /* bClockID */
                        g_state.is_uac2 = 1;
                        LOGI("Found UAC2 Clock Source ID: %d", g_state.clock_source_id);
                    }
                    /* UAC1/UAC2: FEATURE_UNIT (0x06) */
                    if (bDescriptorSubtype == 0x06 && bLength >= 4) {
                        g_state.feature_unit_id = extra[3];  /* bUnitID */
                        LOGI("Found Feature Unit ID: %d", g_state.feature_unit_id);
                    }
                }

                extra += bLength;
                extra_len -= bLength;
            }
        }
    }

    libusb_free_config_descriptor(config);

    if (g_state.ep_out == 0) {
        LOGE("No suitable isochronous OUT endpoint found");
        return -1;
    }

    LOGI("Selected: interface=%d, alt=%d, ep=0x%02X, maxPacket=%d, UAC2=%d",
         g_state.iface_num, g_state.alt_setting, g_state.ep_out,
         g_state.ep_max_packet_size, g_state.is_uac2);

    return 0;
}

/* ============================================================
 * Sample Rate / Format Control
 * ============================================================ */

static int set_sample_rate_uac1(int sample_rate) {
    /* UAC1: Set sample rate via Class-Specific Endpoint Request */
    uint8_t data[3];
    data[0] = sample_rate & 0xFF;
    data[1] = (sample_rate >> 8) & 0xFF;
    data[2] = (sample_rate >> 16) & 0xFF;

    int rc = libusb_control_transfer(g_state.devh,
        0x22,  /* bmRequestType: Host-to-device, Class, Endpoint */
        UAC_SET_CUR,
        UAC_EP_CS_ATTR_SAMPLE_RATE << 8,  /* wValue: Sampling Frequency Control */
        g_state.ep_out,                    /* wIndex: endpoint address */
        data, 3, 1000);

    if (rc < 0) {
        LOGE("UAC1 set_sample_rate failed: %d (%s)", rc, libusb_error_name(rc));
        return -1;
    }

    LOGI("UAC1: Set sample rate to %d Hz", sample_rate);
    return 0;
}

static int set_sample_rate_uac2(int sample_rate) {
    /* UAC2: Set sample rate via Clock Source Control Request */
    if (g_state.clock_source_id < 0) {
        LOGE("No Clock Source ID found for UAC2");
        return -1;
    }

    uint8_t data[4];
    data[0] = sample_rate & 0xFF;
    data[1] = (sample_rate >> 8) & 0xFF;
    data[2] = (sample_rate >> 16) & 0xFF;
    data[3] = (sample_rate >> 24) & 0xFF;

    /* bmRequestType 0x21 = Host-to-device | Class | Interface (NOT endpoint).
     * wValue  = Control Selector (SAM_FREQ) in high byte, channel 0 in low byte.
     * wIndex  = Entity ID (Clock Source) in HIGH byte, AudioControl interface in
     *           LOW byte.  (The previous code used 0x22/endpoint and swapped the
     *           wIndex bytes, which is why the DAC's clock was never set.) */
    int rc = libusb_control_transfer(g_state.devh,
        0x21,  /* bmRequestType: H2D, Class, Interface */
        UAC_SET_CUR,
        UAC2_CS_CONTROL_SAM_FREQ << 8,
        (g_state.clock_source_id << 8) | g_state.ac_iface_num,
        data, 4, 1000);

    if (rc < 0) {
        LOGE("UAC2 set_sample_rate failed: %d (%s)", rc, libusb_error_name(rc));
        return -1;
    }

    LOGI("UAC2: Set sample rate to %d Hz via Clock Source %d", sample_rate, g_state.clock_source_id);
    return 0;
}

static int set_sample_rate(int sample_rate) {
    if (g_state.is_uac2) {
        return set_sample_rate_uac2(sample_rate);
    } else {
        return set_sample_rate_uac1(sample_rate);
    }
}

/* ============================================================
 * Volume Control
 * ============================================================ */

static int set_volume_db(float volume_db) {
    if (g_state.feature_unit_id < 0) {
        LOGW("No Feature Unit found, volume control not available");
        return -1;
    }

    /* Convert dB to raw value (typically 0.25 dB steps, signed 16-bit) */
    int16_t raw_val = (int16_t)(volume_db * 256.0f);  /* 1/256 dB units */
    uint8_t data[2];
    data[0] = raw_val & 0xFF;
    data[1] = (raw_val >> 8) & 0xFF;

    /* Set master-channel volume. wIndex = Entity ID (HIGH) | AC interface (LOW). */
    int rc = libusb_control_transfer(g_state.devh,
        0x21,  /* Host-to-device, Class, Interface */
        UAC_SET_CUR,
        UAC_FU_VOLUME << 8,               /* wValue: Volume Control, channel 0 */
        (g_state.feature_unit_id << 8) | g_state.ac_iface_num,  /* wIndex */
        data, 2, 1000);

    if (rc < 0) {
        LOGW("Set volume failed: %d", rc);
        return -1;
    }

    LOGI("Set volume to %.1f dB (raw: %d)", volume_db, raw_val);
    return 0;
}

static int set_mute(int mute) {
    if (g_state.feature_unit_id < 0) return -1;

    uint8_t data = mute ? 1 : 0;
    int rc = libusb_control_transfer(g_state.devh,
        0x21, UAC_SET_CUR,
        UAC_FU_MUTE << 8,
        (g_state.feature_unit_id << 8) | g_state.ac_iface_num,
        &data, 1, 1000);

    return (rc < 0) ? -1 : 0;
}

/* ============================================================
 * Isochronous OUT Transfer Engine
 * ============================================================ */

/* Compute how the audio stream maps onto isochronous packets. Isochronous has
 * no flow control: each service interval we must send EXACTLY the number of
 * bytes the sink consumes at the configured sample rate. */
static void compute_packet_sizing(void) {
    g_state.bytes_per_frame = (g_state.bit_depth / 8) * g_state.channels;
    if (g_state.bytes_per_frame <= 0) g_state.bytes_per_frame = 4;

    int intv = g_state.ep_interval > 0 ? g_state.ep_interval : 1;

    /* libusb_get_device_speed() often returns UNKNOWN for an fd wrapped via
     * libusb_wrap_sys_device on Android. Modern USB DAC dongles ("小尾巴") are
     * essentially always USB 2.0 high speed, so default UNKNOWN to HIGH —
     * guessing full speed here would size every packet 8x wrong and produce
     * near-total underrun (silence). */
    int speed = g_state.dev_speed;
    if (speed == LIBUSB_SPEED_UNKNOWN) {
        speed = LIBUSB_SPEED_HIGH;
        LOGW("Device speed unknown (wrapped fd); assuming HIGH speed");
    }

    if (speed >= LIBUSB_SPEED_HIGH) {
        /* High speed: 8000 microframes/s; period = 2^(bInterval-1) microframes. */
        int period = 1 << (intv - 1);
        if (period < 1) period = 1;
        g_state.packets_per_sec = 8000 / period;
    } else {
        /* Full speed: 1000 frames/s; period = bInterval frames. */
        g_state.packets_per_sec = 1000 / intv;
    }
    if (g_state.packets_per_sec < 1) g_state.packets_per_sec = 1000;

    g_state.frames_per_packet = g_state.sample_rate / g_state.packets_per_sec;
    g_state.frames_remainder  = g_state.sample_rate % g_state.packets_per_sec;
    g_state.frac_acc = 0;
    g_state.fb_acc = 0;
    g_state.fb_samples_per_pkt = 0;  /* until first feedback arrives, use nominal */

    /* Build ~50ms of cushion before real output so capture bursts / scheduling
     * jitter don't drain the ring to empty (the residual micro-dropouts). */
    g_state.primed = 0;
    int bps = g_state.sample_rate * g_state.bytes_per_frame;
    g_state.prime_threshold = bps / 20;  /* 50 ms */
    if (g_state.prime_threshold > g_state.ring_size / 2)
        g_state.prime_threshold = g_state.ring_size / 2;

    int max_bytes = (g_state.frames_per_packet + 1) * g_state.bytes_per_frame;
    if (g_state.ep_max_packet_size > 0 && max_bytes > g_state.ep_max_packet_size)
        max_bytes = g_state.ep_max_packet_size;
    g_state.packet_stride = max_bytes;

    LOGI("Packet sizing: speed=%d intv=%d -> %d pkt/s, %d frame/pkt (rem %d), "
         "bpf=%d stride=%d epmax=%d",
         g_state.dev_speed, intv, g_state.packets_per_sec,
         g_state.frames_per_packet, g_state.frames_remainder,
         g_state.bytes_per_frame, g_state.packet_stride, g_state.ep_max_packet_size);
}

/* Whole frames for the next packet. The target rate is the device's asynchronous
 * feedback value when available (kept within ±10% of nominal as a safety net
 * against a misparsed feedback word), otherwise the fixed nominal. A double
 * accumulator distributes the fractional part exactly. */
static int next_packet_frames(void) {
    double nominal = (double)g_state.sample_rate / g_state.packets_per_sec;
    double spp = g_state.fb_samples_per_pkt;
    if (spp < nominal * 0.9 || spp > nominal * 1.1) spp = nominal;

    g_state.fb_acc += spp;
    int frames = (int)g_state.fb_acc;
    g_state.fb_acc -= frames;
    return frames;
}

/* Set per-packet lengths and copy PCM (or silence on underrun) into the packed
 * iso buffer. Uses libusb_get_iso_packet_buffer (cumulative offsets), so all
 * lengths must be assigned before any buffer pointer is taken. */
static int fill_iso_packets(struct libusb_transfer *xfr) {
    for (int i = 0; i < xfr->num_iso_packets; i++) {
        int len = next_packet_frames() * g_state.bytes_per_frame;
        if (len > g_state.packet_stride) len = g_state.packet_stride;
        xfr->iso_packet_desc[i].length = len;
    }

    /* While priming, emit silence and let the ring fill to the cushion target. */
    int priming = 0;
    if (!g_state.primed) {
        if (ring_buffer_available_read() < g_state.prime_threshold) priming = 1;
        else g_state.primed = 1;
    }

    int total = 0;
    for (int i = 0; i < xfr->num_iso_packets; i++) {
        int len = xfr->iso_packet_desc[i].length;
        uint8_t *buf = libusb_get_iso_packet_buffer(xfr, i);
        if (priming) {
            memset(buf, 0, len);
        } else {
            int n = ring_buffer_read(buf, len);
            if (n < len) {
                memset(buf + n, 0, len - n);   /* underrun -> silence */
                g_state.underrun_count++;
            }
        }
        total += len;
    }
    return total;
}

static void iso_out_callback(struct libusb_transfer *xfr) {
    if (xfr->status != LIBUSB_TRANSFER_COMPLETED) {
        if (xfr->status == LIBUSB_TRANSFER_CANCELLED) {
            return;  /* Normal shutdown */
        }
        LOGW("OUT transfer status: %d", xfr->status);
    }

    if (!g_state.running) return;

    g_state.bytes_written += fill_iso_packets(xfr);

    /* Throttled stats so we can tell from logs whether audio is really flowing. */
    static unsigned long cb_count = 0;
    if ((++cb_count % 500) == 0) {
        LOGI("flowing: bytes=%lu underruns=%lu ringAvail=%d fb=%.3f",
             g_state.bytes_written, g_state.underrun_count,
             ring_buffer_available_read(), g_state.fb_samples_per_pkt);
    }

    int rc = libusb_submit_transfer(xfr);
    if (rc < 0) {
        LOGE("Failed to re-submit OUT transfer: %d (%s)", rc, libusb_error_name(rc));
    }
}

/* Asynchronous feedback: the device reports, in Q16.16 (high speed) or Q10.14
 * (full speed) format, how many samples per (micro)frame it actually wants. We
 * track that as fb_samples_per_pkt so next_packet_frames() sends exactly the
 * device's rate, keeping its internal FIFO from drifting (the cause of periodic
 * dropouts when feedback is ignored). */
static void fb_in_callback(struct libusb_transfer *xfr) {
    if (xfr->status == LIBUSB_TRANSFER_CANCELLED || !g_state.running) return;

    if (xfr->num_iso_packets > 0) {
        struct libusb_iso_packet_descriptor *p = &xfr->iso_packet_desc[0];
        if (p->status == LIBUSB_TRANSFER_COMPLETED && p->actual_length >= 3) {
            uint8_t *b = libusb_get_iso_packet_buffer_simple(xfr, 0);
            double spp;
            if (p->actual_length >= 4) {
                uint32_t v = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
                spp = v / 65536.0;          /* high speed: samples per microframe */
            } else {
                uint32_t v = b[0] | (b[1] << 8) | (b[2] << 16);
                spp = v / 16384.0;          /* full speed: samples per frame */
            }
            if (spp > 0) g_state.fb_samples_per_pkt = spp;
        }
    }

    if (g_state.running) libusb_submit_transfer(xfr);
}

static void *event_loop(void *arg) {
    (void)arg;
    /* Audio I/O thread: raise priority so libusb completions are serviced and
     * resubmitted promptly, avoiding gaps in the isochronous stream. */
    setpriority(PRIO_PROCESS, 0, -19);
    LOGI("USB event loop started");

    while (g_state.running) {
        struct timeval tv = {0, 100000};  /* 100ms timeout */
        int rc = libusb_handle_events_timeout(g_state.ctx, &tv);
        if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_INTERRUPTED) {
            LOGE("libusb event error: %d", rc);
            break;
        }
    }

    LOGI("USB event loop stopped");
    return NULL;
}

/* Allocate and submit the asynchronous feedback IN transfers. Non-fatal: if the
 * device has no feedback endpoint or submission fails, we fall back to nominal. */
static void start_feedback(void) {
    if (g_state.ep_feedback == 0) {
        LOGI("No feedback endpoint; using nominal rate");
        return;
    }
    int mps = g_state.ep_feedback_mps > 0 ? g_state.ep_feedback_mps : 4;
    for (int i = 0; i < NUM_FB_TRANSFERS; i++) {
        g_state.fb_buffers[i] = (uint8_t *)calloc(mps, 1);
        g_state.fb_transfers[i] = libusb_alloc_transfer(1);
        if (!g_state.fb_buffers[i] || !g_state.fb_transfers[i]) {
            LOGW("Feedback transfer %d alloc failed", i);
            continue;
        }
        libusb_fill_iso_transfer(g_state.fb_transfers[i], g_state.devh,
            g_state.ep_feedback, g_state.fb_buffers[i], mps, 1,
            fb_in_callback, NULL, 0);
        libusb_set_iso_packet_lengths(g_state.fb_transfers[i], mps);
        int rc = libusb_submit_transfer(g_state.fb_transfers[i]);
        if (rc < 0) LOGW("Feedback submit %d failed: %s", i, libusb_error_name(rc));
    }
    LOGI("Feedback servicing started on EP 0x%02X", g_state.ep_feedback);
}

static int start_iso_output(void) {
    int num_iso_packets = ISO_PACKETS_PER_XFR;

    compute_packet_sizing();
    int buf_size = g_state.packet_stride * num_iso_packets;

    /* Submitting pulls from the ring buffer immediately; mark running first so
     * the callbacks (which check g_state.running) keep resubmitting. */
    g_state.running = 1;

    for (int i = 0; i < NUM_OUT_TRANSFERS; i++) {
        g_state.transfer_buffers[i] = (uint8_t *)calloc(buf_size, 1);
        if (!g_state.transfer_buffers[i]) {
            LOGE("Failed to allocate transfer buffer %d", i);
            g_state.running = 0;
            return -ENOMEM;
        }

        g_state.out_transfers[i] = libusb_alloc_transfer(num_iso_packets);
        if (!g_state.out_transfers[i]) {
            LOGE("Failed to allocate transfer %d", i);
            g_state.running = 0;
            return -ENOMEM;
        }

        libusb_fill_iso_transfer(
            g_state.out_transfers[i],
            g_state.devh,
            g_state.ep_out,
            g_state.transfer_buffers[i],
            buf_size,
            num_iso_packets,
            iso_out_callback,
            NULL,
            1000  /* timeout ms */
        );

        /* Set per-packet lengths (and prime with silence/PCM) for this rate. */
        fill_iso_packets(g_state.out_transfers[i]);

        int rc = libusb_submit_transfer(g_state.out_transfers[i]);
        if (rc < 0) {
            LOGE("Failed to submit transfer %d: %s", i, libusb_error_name(rc));
            g_state.running = 0;
            return rc;
        }
    }

    /* Service the device's asynchronous feedback so we send its exact rate. */
    start_feedback();

    gettimeofday(&g_state.start_time, NULL);

    /* Start event thread */
    pthread_create(&g_state.event_thread, NULL, event_loop, NULL);

    LOGI("Isochronous OUT output started with %d transfers", NUM_OUT_TRANSFERS);
    return 0;
}

static void stop_iso_output(void) {
    g_state.running = 0;

    /* Wait for event thread to finish */
    if (g_state.event_thread) {
        pthread_join(g_state.event_thread, NULL);
        g_state.event_thread = 0;
    }

    /* Cancel and free transfers (data OUT + feedback IN) */
    for (int i = 0; i < NUM_OUT_TRANSFERS; i++) {
        if (g_state.out_transfers[i]) {
            libusb_cancel_transfer(g_state.out_transfers[i]);
        }
    }
    for (int i = 0; i < NUM_FB_TRANSFERS; i++) {
        if (g_state.fb_transfers[i]) {
            libusb_cancel_transfer(g_state.fb_transfers[i]);
        }
    }

    /* Process any remaining events */
    struct timeval tv = {0, 100000};
    libusb_handle_events_timeout(g_state.ctx, &tv);

    for (int i = 0; i < NUM_OUT_TRANSFERS; i++) {
        if (g_state.out_transfers[i]) {
            libusb_free_transfer(g_state.out_transfers[i]);
            g_state.out_transfers[i] = NULL;
        }
        if (g_state.transfer_buffers[i]) {
            free(g_state.transfer_buffers[i]);
            g_state.transfer_buffers[i] = NULL;
        }
    }
    for (int i = 0; i < NUM_FB_TRANSFERS; i++) {
        if (g_state.fb_transfers[i]) {
            libusb_free_transfer(g_state.fb_transfers[i]);
            g_state.fb_transfers[i] = NULL;
        }
        if (g_state.fb_buffers[i]) {
            free(g_state.fb_buffers[i]);
            g_state.fb_buffers[i] = NULL;
        }
    }

    LOGI("Output stopped. Bytes written: %lu, Underruns: %lu",
         g_state.bytes_written, g_state.underrun_count);
}

/* ============================================================
 * JNI Functions
 * ============================================================ */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    LOGI("USB Audio Output library loaded");
    memset(&g_state, 0, sizeof(g_state));
    g_state.java_vm = vm;
    pthread_mutex_init(&g_state.ring_mutex, NULL);
    pthread_cond_init(&g_state.ring_cond, NULL);
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
    (void)vm; (void)reserved;
    if (g_state.callback_class) {
        JNIEnv *env;
        if ((*g_state.java_vm)->GetEnv(g_state.java_vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) {
            (*env)->DeleteGlobalRef(env, g_state.callback_class);
        }
    }
    LOGI("USB Audio Output library unloaded");
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Probe the device's supported PCM formats without starting a stream.
 * Returns a string: "bits=16,24,32;rates=44100,48000,96000,192000".
 * The Android side must have already claimed the audio interfaces (needed for
 * the UAC2 clock GET_RANGE control transfer).
 */
JNIEXPORT jstring JNICALL
Java_com_salt_usb_audio_UsbAudioOutput_nativeProbe(
    JNIEnv *env, jobject thiz, jint fileDescriptor) {
    (void)thiz;

    char result[512];
    result[0] = '\0';

    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) return (*env)->NewStringUTF(env, "");
    libusb_device_handle *devh = NULL;
    if (libusb_wrap_sys_device(ctx, (intptr_t)fileDescriptor, &devh) < 0 || !devh) {
        libusb_exit(ctx);
        return (*env)->NewStringUTF(env, "");
    }

    libusb_device *dev = libusb_get_device(devh);
    struct libusb_config_descriptor *config = NULL;
    if (libusb_get_active_config_descriptor(dev, &config) < 0) {
        libusb_close(devh); libusb_exit(ctx);
        return (*env)->NewStringUTF(env, "");
    }

    int bits_seen[4] = {0,0,0,0};   /* 16,24,32 flags */
    int clock_id = -1, ac_iface = 0, is_uac2 = 0;

    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &config->interface[i];
        for (int j = 0; j < iface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[j];
            if (alt->bInterfaceClass != USB_CLASS_AUDIO) continue;

            if (alt->bInterfaceSubClass == USB_SUBCLASS_AUDIOSTREAMING) {
                const uint8_t *e = alt->extra; int el = alt->extra_length;
                while (el >= 3) {
                    uint8_t bl = e[0], bt = e[1], bs = e[2];
                    if (bl < 3 || bl > el) break;
                    if (bt == 0x24 && bs == 0x02 && bl >= 6 && e[3] == 0x01) {
                        int bitres = (bl >= 8) ? e[6] : e[5];   /* UAC1 vs UAC2 */
                        if (bitres == 16) bits_seen[0] = 1;
                        else if (bitres == 24) bits_seen[1] = 1;
                        else if (bitres == 32) bits_seen[2] = 1;
                    }
                    e += bl; el -= bl;
                }
            } else if (alt->bInterfaceSubClass == USB_SUBCLASS_AUDIOCONTROL) {
                const uint8_t *e = alt->extra; int el = alt->extra_length;
                while (el >= 3) {
                    uint8_t bl = e[0], bt = e[1], bs = e[2];
                    if (bl < 3 || bl > el) break;
                    if (bt == 0x24 && bs == 0x0A && bl >= 4) {   /* CLOCK_SOURCE */
                        clock_id = e[3]; is_uac2 = 1; ac_iface = alt->bInterfaceNumber;
                    }
                    e += bl; el -= bl;
                }
            }
        }
    }

    /* Build bits= list */
    char bits_str[32]; bits_str[0] = '\0';
    if (bits_seen[0]) strcat(bits_str, "16,");
    if (bits_seen[1]) strcat(bits_str, "24,");
    if (bits_seen[2]) strcat(bits_str, "32,");
    if (bits_str[0] == '\0') strcpy(bits_str, "16,");
    bits_str[strlen(bits_str) - 1] = '\0';   /* trim trailing comma */

    /* Build rates= list: UAC2 via clock GET_RANGE, else a standard set. */
    char rates_str[256]; rates_str[0] = '\0';
    int got_rates = 0;
    if (is_uac2 && clock_id >= 0) {
        uint8_t buf[256];
        int r = libusb_control_transfer(devh, 0xA1, 0x02 /*RANGE*/,
            (0x01 << 8) /*SAM_FREQ*/, (clock_id << 8) | ac_iface,
            buf, sizeof(buf), 1000);
        if (r >= 2) {
            int n = buf[0] | (buf[1] << 8);
            for (int k = 0; k < n; k++) {
                int off = 2 + k * 12;
                if (off + 12 > r) break;
                uint32_t dmin = le32(buf + off);
                char tmp[16];
                snprintf(tmp, sizeof(tmp), "%u,", dmin);
                if (strlen(rates_str) + strlen(tmp) < sizeof(rates_str)) strcat(rates_str, tmp);
                got_rates = 1;
            }
        } else {
            LOGW("Clock GET_RANGE failed: %d (%s)", r, libusb_error_name(r));
        }
    }
    if (!got_rates) {
        strcpy(rates_str, "44100,48000,88200,96000,176400,192000,");
    }
    if (rates_str[0]) rates_str[strlen(rates_str) - 1] = '\0';

    snprintf(result, sizeof(result), "bits=%s;rates=%s;uac2=%d", bits_str, rates_str, is_uac2);
    LOGI("Probe: %s", result);

    libusb_free_config_descriptor(config);
    libusb_close(devh);
    libusb_exit(ctx);
    return (*env)->NewStringUTF(env, result);
}

/*
 * Open and initialize USB audio device.
 * @param fileDescriptor  File descriptor from UsbDeviceConnection
 * @param sampleRate      Desired sample rate (e.g., 44100, 48000, 96000)
 * @param bitDepth        Desired bit depth (16, 24, or 32)
 * @param channels        Number of channels (1 or 2)
 * @return                0 on success, negative on error
 */
JNIEXPORT jint JNICALL
Java_com_salt_usb_audio_UsbAudioOutput_nativeOpen(
    JNIEnv *env, jobject thiz,
    jint fileDescriptor,
    jint sampleRate, jint bitDepth, jint channels) {

    LOGI("Opening USB audio device: fd=%d, rate=%d, bits=%d, ch=%d",
         fileDescriptor, sampleRate, bitDepth, channels);

    /* Initialize libusb with no device discovery (we provide the fd) */
    int rc = libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
    if (rc != LIBUSB_SUCCESS) {
        LOGE("libusb_set_option failed: %d", rc);
        return -1;
    }

    rc = libusb_init(&g_state.ctx);
    if (rc < 0) {
        LOGE("libusb_init failed: %d", rc);
        return -2;
    }

    /* Wrap the Android USB file descriptor */
    rc = libusb_wrap_sys_device(g_state.ctx, (intptr_t)fileDescriptor, &g_state.devh);
    if (rc < 0 || !g_state.devh) {
        LOGE("libusb_wrap_sys_device failed: %d", rc);
        libusb_exit(g_state.ctx);
        g_state.ctx = NULL;
        return -3;
    }

    /* Get device info */
    libusb_device *dev = libusb_get_device(g_state.devh);
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor(dev, &desc);
    g_state.vendor_id = desc.idVendor;
    g_state.product_id = desc.idProduct;

    /* Get device name */
    if (desc.iProduct) {
        libusb_get_string_descriptor_ascii(g_state.devh, desc.iProduct,
            (unsigned char *)g_state.device_name, sizeof(g_state.device_name));
    }
    if (desc.iManufacturer) {
        libusb_get_string_descriptor_ascii(g_state.devh, desc.iManufacturer,
            (unsigned char *)g_state.manufacturer, sizeof(g_state.manufacturer));
    }

    LOGI("Device: [%s] %s (VID=%04X PID=%04X)",
         g_state.manufacturer, g_state.device_name,
         g_state.vendor_id, g_state.product_id);

    /* Record requested format, then let descriptor parsing pick the matching
     * alternate setting (it may refine bit_depth/channels to the device's). */
    g_state.bit_depth = bitDepth;
    g_state.channels = channels;
    g_state.dev_speed = libusb_get_device_speed(dev);
    LOGI("Device speed code: %d (1=low 2=full 3=high 4=super)", g_state.dev_speed);

    /* Parse USB Audio Class descriptors */
    rc = parse_uac_descriptors(bitDepth, channels);
    if (rc < 0) {
        LOGE("Failed to find suitable audio streaming interface");
        libusb_close(g_state.devh);
        libusb_exit(g_state.ctx);
        g_state.devh = NULL;
        g_state.ctx = NULL;
        return -4;
    }

    /* Detach kernel driver if active.
     * On Android there is usually no kernel driver in the classic sense, and the
     * Java side may already have taken the interface via
     * UsbDeviceConnection.claimInterface(force=true). Treat failures here as
     * non-fatal: if we cannot proceed, the claim below will tell us. */
    int kd = libusb_kernel_driver_active(g_state.devh, g_state.iface_num);
    if (kd == 1) {
        rc = libusb_detach_kernel_driver(g_state.devh, g_state.iface_num);
        if (rc < 0) {
            LOGW("Could not detach kernel driver (continuing): %s", libusb_error_name(rc));
        } else {
            LOGI("Kernel driver detached");
        }
    } else if (kd < 0) {
        LOGW("kernel_driver_active query failed (continuing): %s", libusb_error_name(kd));
    }

    /* Claim the interface exclusively.
     * If the Java layer already claimed it at the Android level, libusb may
     * report BUSY here even though we effectively own the interface through the
     * shared fd. Tolerate BUSY and continue; only hard-fail on other errors. */
    rc = libusb_claim_interface(g_state.devh, g_state.iface_num);
    if (rc == LIBUSB_ERROR_BUSY) {
        LOGW("Interface %d already claimed at Android level; continuing", g_state.iface_num);
    } else if (rc < 0) {
        LOGE("Could not claim interface: %s", libusb_error_name(rc));
        libusb_close(g_state.devh);
        libusb_exit(g_state.ctx);
        g_state.devh = NULL;
        g_state.ctx = NULL;
        return -6;
    } else {
        LOGI("Interface %d claimed exclusively", g_state.iface_num);
    }

    /* Make sure we start from the zero-bandwidth alt while configuring. */
    libusb_set_interface_alt_setting(g_state.devh, g_state.iface_num, 0);

    /* Set the clock sample rate BEFORE selecting the streaming alt setting.
     * Doing it while the interface is still in alt 0 avoids changing the clock
     * mid-stream, which some DACs respond to by re-enumerating. */
    rc = set_sample_rate(sampleRate);
    if (rc < 0) {
        LOGW("Failed to set sample rate (device may not support this rate)");
    }
    /* We feed data at this rate regardless; packet sizing depends on it. */
    g_state.sample_rate = sampleRate;

    /* Now select the streaming alternate setting (selects the audio format). */
    rc = libusb_set_interface_alt_setting(g_state.devh, g_state.iface_num, g_state.alt_setting);
    if (rc < 0) {
        LOGE("Could not set alt setting: %s", libusb_error_name(rc));
        /* Try with alt setting 1 as fallback */
        g_state.alt_setting = 1;
        rc = libusb_set_interface_alt_setting(g_state.devh, g_state.iface_num, g_state.alt_setting);
        if (rc < 0) {
            LOGE("Fallback alt setting also failed");
            libusb_release_interface(g_state.devh, g_state.iface_num);
            libusb_close(g_state.devh);
            libusb_exit(g_state.ctx);
            g_state.devh = NULL;
            g_state.ctx = NULL;
            return -7;
        }
    }
    LOGI("Alt setting %d selected", g_state.alt_setting);

    /* bit_depth / channels were set from the matched alt in parse_uac_descriptors;
     * warn if the device's format differs from what the caller will feed. */
    if (g_state.bit_depth != bitDepth || g_state.channels != channels) {
        LOGW("Requested %dbit/%dch but selected alt provides %dbit/%dch — "
             "PCM data format may not match the device!",
             bitDepth, channels, g_state.bit_depth, g_state.channels);
    }

    /* Allocate ring buffer */
    g_state.ring_size = RING_BUFFER_SIZE;
    g_state.ring_buffer = (uint8_t *)calloc(g_state.ring_size, 1);
    if (!g_state.ring_buffer) {
        LOGE("Failed to allocate ring buffer");
        libusb_release_interface(g_state.devh, g_state.iface_num);
        libusb_close(g_state.devh);
        libusb_exit(g_state.ctx);
        g_state.devh = NULL;
        g_state.ctx = NULL;
        return -8;
    }
    g_state.ring_read_pos = 0;
    g_state.ring_write_pos = 0;

    LOGI("USB audio device opened successfully");
    return 0;
}

/*
 * Start audio output.
 */
JNIEXPORT jint JNICALL
Java_com_salt_usb_audio_UsbAudioOutput_nativeStart(
    JNIEnv *env, jobject thiz) {

    if (!g_state.devh) {
        LOGE("Device not opened");
        return -1;
    }

    g_state.bytes_written = 0;
    g_state.underrun_count = 0;

    return start_iso_output();
}

/*
 * Feed PCM data to the USB output.
 * @param data     PCM audio data (interleaved, little-endian)
 * @param offset   Starting offset in data array
 * @param length   Number of bytes to write
 * @return         Number of bytes actually written
 */
JNIEXPORT jint JNICALL
Java_com_salt_usb_audio_UsbAudioOutput_nativeWrite(
    JNIEnv *env, jobject thiz,
    jbyteArray data, jint offset, jint length) {

    if (!g_state.running) return -1;

    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (!bytes) return -1;

    int available = ring_buffer_available_write();
    int write_len = (length < available) ? length : available;

    if (write_len <= 0) {
        (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
        return 0;
    }

    /* Write to ring buffer */
    int write_pos = g_state.ring_write_pos;
    int first_part = g_state.ring_size - write_pos;
    if (first_part > write_len) first_part = write_len;

    memcpy(g_state.ring_buffer + write_pos, bytes + offset, first_part);
    if (first_part < write_len) {
        memcpy(g_state.ring_buffer, bytes + offset + first_part, write_len - first_part);
    }

    g_state.ring_write_pos = (write_pos + write_len) % g_state.ring_size;

    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
    return write_len;
}

/*
 * Stop audio output.
 */
JNIEXPORT void JNICALL
Java_com_salt_usb_audio_UsbAudioOutput_nativeStop(
    JNIEnv *env, jobject thiz) {

    if (g_state.running) {
        stop_iso_output();
    }
}

/*
 * Close and release USB device.
 */
JNIEXPORT void JNICALL
Java_com_salt_usb_audio_UsbAudioOutput_nativeClose(
    JNIEnv *env, jobject thiz) {

    if (g_state.running) {
        stop_iso_output();
    }

    if (g_state.ring_buffer) {
        free(g_state.ring_buffer);
        g_state.ring_buffer = NULL;
    }

    if (g_state.devh) {
        /* Restore alt setting 0 (stop streaming bandwidth). */
        libusb_set_interface_alt_setting(g_state.devh, g_state.iface_num, 0);
        libusb_release_interface(g_state.devh, g_state.iface_num);

        /* Reset the device so it re-enumerates on the USB bus. Without this,
         * after our exclusive claim Android often won't re-grab the DAC and
         * normal system playback stays dead until the user replugs it. The
         * reset triggers detach+attach, so AudioService re-attaches its driver
         * and routes audio to the DAC again. The handle is invalid afterwards. */
        int rc = libusb_reset_device(g_state.devh);
        LOGI("libusb_reset_device -> %d (%s)", rc, libusb_error_name(rc));

        libusb_close(g_state.devh);
        g_state.devh = NULL;
    }

    if (g_state.ctx) {
        libusb_exit(g_state.ctx);
        g_state.ctx = NULL;
    }

    LOGI("USB audio device closed");
}

/*
 * Set volume in dB.
 */
JNIEXPORT jint JNICALL
Java_com_salt_usb_audio_UsbAudioOutput_nativeSetVolume(
    JNIEnv *env, jobject thiz, jfloat volumeDb) {
    return set_volume_db(volumeDb);
}

/*
 * Get device info as a string.
 */
JNIEXPORT jstring JNICALL
Java_com_salt_usb_audio_UsbAudioOutput_nativeGetDeviceInfo(
    JNIEnv *env, jobject thiz) {

    char info[512];
    snprintf(info, sizeof(info),
             "%s %s (VID:%04X PID:%04X) Rate:%d Bit:%d Ch:%d UAC2:%s EP:0x%02X",
             g_state.manufacturer, g_state.device_name,
             g_state.vendor_id, g_state.product_id,
             g_state.sample_rate, g_state.bit_depth, g_state.channels,
             g_state.is_uac2 ? "yes" : "no",
             g_state.ep_out);

    return (*env)->NewStringUTF(env, info);
}

/*
 * Get buffer fill level (0.0 - 1.0).
 */
JNIEXPORT jfloat JNICALL
Java_com_salt_usb_audio_UsbAudioOutput_nativeGetBufferLevel(
    JNIEnv *env, jobject thiz) {

    if (!g_state.ring_buffer) return 0.0f;
    return (float)ring_buffer_available_read() / (float)g_state.ring_size;
}
