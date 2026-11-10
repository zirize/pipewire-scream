/* PipeWire Scream Sender Module
 *
 * Creates a virtual audio sink and transmits audio over UDP using the Scream protocol.
 * Compatible with existing Scream receivers.
 *
 * Based on module-null-sink and the Scream protocol specification.
 *
 * Copyright (c) 2015 Microsoft (original Scream project)
 * Copyright (c) 2026 pipewire-scream contributors (PipeWire module implementation)
 *
 * This project is a derivative work of the Scream project
 * (https://github.com/duncanthrax/scream)
 *
 * Licensed under the Microsoft Public License (MS-PL).
 * See LICENSE file for full license text.
 */

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>
#include <pipewire/extensions/metadata.h>

/* Version compatibility: PipeWire 1.0 removed media_type/media_subtype from spa_audio_info_raw */
#if !PW_CHECK_VERSION(1, 0, 0)
#define PW_LEGACY_API
#endif

/* Format constant naming changed in PipeWire 1.0: old uses S16LE, new uses S16_LE */
#ifdef SPA_AUDIO_FORMAT_S16LE
/* PipeWire < 1.0: use old constant names */
#define SCREAM_AUDIO_FORMAT SPA_AUDIO_FORMAT_S16LE
#else
/* PipeWire >= 1.0: use new constant names */
#define SCREAM_AUDIO_FORMAT SPA_AUDIO_FORMAT_S16_LE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <time.h>

#define MODULE_NAME "module-scream"
#define DEFAULT_SINK_NAME "Scream"
#define DEFAULT_SINK_DESCRIPTION "Scream Network Sink"

/* Scream protocol constants */
#define SCREAM_MULTICAST_GROUP "239.255.77.77"
#define SCREAM_DEFAULT_PORT 4010
#define SCREAM_HEADER_SIZE 5
#define SCREAM_MAX_PAYLOAD 1152

/* Configuration limits */
#define MIN_SAMPLE_RATE 8000
#define MAX_SAMPLE_RATE 384000
#define MIN_CHANNELS 1
#define MAX_CHANNELS 255
#define MIN_PORT 1
#define MAX_PORT 65535
#define DEFAULT_MULTICAST_TTL 1
#define MAX_MULTICAST_TTL 255
#define PARAM_BUFFER_SIZE 1024
#define MAX_CONSECUTIVE_SEND_FAILURES 100

/* Silence suppression: stop transmitting after this many consecutive silent frames.
 * 24000 frames is half a second at 48 kHz. Set silence.threshold=0 to transmit always. */
#define DEFAULT_SILENCE_FRAMES 24000
#define MAX_SILENCE_FRAMES 100000000L

/* Control protocol (unicast button, auto output) - see TECHNICAL.md "Control protocol".
 * Off unless discovery.port is set. Never touches the audio wire format. */
#define CTRL_MAGIC "SCRMDRD1"
#define CTRL_MAGIC_LEN 8
#define CTRL_QUERY 1
#define CTRL_TAKE 2
#define CTRL_RELEASE 3
#define CTRL_ANSWER 4
#define CTRL_NOTICE 5
#define CTRL_CONNECT 6
#define CTRL_DISCONNECT 7
#define CTRL_KEEPALIVE 8
#define CTRL_NOTICE_TAKEN 1
#define CTRL_NOTICE_OUTPUT_MOVED 2
#define CTRL_NAME_MAX 32
#define CTRL_MAX_PACKET 64
#define CTRL_MAX_CONNECTED 4
#define CTRL_MAX_ALLOW 8
#define CTRL_SENDING_WINDOW_NS 1000000000ULL
#define NODE_NAME_MAX 256

/* Sample rate encoding for Scream protocol */
#define RATE_BASE_48K 0
#define RATE_BASE_44K 1

/* Parse integer parameter with validation */
static int parse_int_param(const char *str, const char *param_name, 
                           long min_val, long max_val, long *result)
{
    char *endptr;
    long value;
    
    if (!str) {
        return -EINVAL;
    }
    
    errno = 0;
    value = strtol(str, &endptr, 10);
    
    if (errno != 0) {
        pw_log_error("Failed to parse %s: %s", param_name, strerror(errno));
        return -errno;
    }
    
    if (*endptr != '\0') {
        pw_log_error("Invalid %s (not a number): %s", param_name, str);
        return -EINVAL;
    }
    
    if (value < min_val || value > max_val) {
        pw_log_error("Invalid %s %ld (must be %ld-%ld)", 
                     param_name, value, min_val, max_val);
        return -EINVAL;
    }
    
    *result = value;
    return 0;
}

struct scream_sink_data {
    struct pw_impl_module *module;
    struct pw_context *context;
    struct spa_hook module_listener;
    
    struct pw_core *core;
    struct spa_hook core_listener;
    
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    
    /* Network configuration */
    int sockfd;
    struct sockaddr_in dest_addr;
    char *ip;
    int port;
    char *interface;
    unsigned char multicast_ttl;
    
    /* Sink properties */
    char *sink_name;
    char *sink_description;
    
    /* Audio format */
    struct spa_audio_info_raw format;
    
    /* Scream protocol state */
    uint8_t packet_buffer[SCREAM_HEADER_SIZE + SCREAM_MAX_PAYLOAD];
    
    /* Silence suppression */
    uint64_t silence_threshold;   /* consecutive silent frames before we stop; 0 = never stop */
    uint64_t silent_frames;       /* how many we have seen in a row */
    bool suppressing;             /* currently withholding packets (for one-shot logging) */
    
    /* Error tracking */
    unsigned int consecutive_send_failures;
    unsigned int dropped_would_block;   /* send buffer was full; packet dropped */

    /* Where the RT thread sends: s_addr << 16 | sin_port, both in network order.
     * Written on the main loop, read once per buffer in on_stream_process(). One atomic
     * word, so the RT side never takes a lock and never sees half an address. */
    _Atomic uint64_t dest_packed;
    _Atomic uint64_t last_send_ns;      /* CLOCK_MONOTONIC of the last packet put on the wire */

    /* Control protocol - everything below lives on the main loop only */
    int ctrl_port;                      /* 0 = feature off */
    int ctrl_fd;
    struct spa_source *ctrl_source;
    uint32_t sender_id;
    char nickname[CTRL_NAME_MAX + 1];
    bool auto_output;
    struct { uint32_t net, mask; } allow[CTRL_MAX_ALLOW];
    int n_allow;

    /* unicast button */
    bool uni_active;
    struct sockaddr_in uni_ctrl;        /* the phone's control socket (answers, notices) */
    uint16_t uni_audio_port;            /* host order */

    /* default output watch */
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct pw_metadata *metadata;
    uint32_t metadata_id;
    struct spa_hook metadata_listener;
    bool is_default;
    int default_expect;                    /* -1, or what we just asked WirePlumber for (0/1) */
    char configured_sink[NODE_NAME_MAX];   /* default.configured.audio.sink */
    char effective_sink[NODE_NAME_MAX];    /* default.audio.sink */

    /* auto output */
    struct { uint32_t instance; struct sockaddr_in addr; } connected[CTRL_MAX_CONNECTED];
    int n_connected;
    char remembered_sink[NODE_NAME_MAX];   /* "" = nothing to restore */
};

static inline uint64_t pack_dest(const struct sockaddr_in *a)
{
    return ((uint64_t)a->sin_addr.s_addr << 16) | (uint64_t)a->sin_port;
}

static inline uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Module properties */
static const struct spa_dict_item module_props[] = {
    { PW_KEY_MODULE_AUTHOR, "Scream Contributors" },
    { PW_KEY_MODULE_DESCRIPTION, "Scream network audio sender" },
    { PW_KEY_MODULE_VERSION, "1.0.0" },
};

/* Encode sample rate for Scream protocol header */
static uint8_t encode_sample_rate(uint32_t rate)
{
    uint8_t base, multiplier;
    
    /* Determine base rate: 48kHz (0) or 44.1kHz (1) */
    if (rate % 44100 == 0) {
        base = RATE_BASE_44K;
        multiplier = rate / 44100;
    } else {
        base = RATE_BASE_48K;
        multiplier = rate / 48000;
    }
    
    /* Clamp multiplier to valid range (1-127) */
    if (multiplier < 1) multiplier = 1;
    if (multiplier > 127) multiplier = 127;
    
    /* Combine: bit 7 = base, bits 0-6 = multiplier */
    return (base << 7) | (multiplier & 0x7F);
}

/* Scream protocol channel masks */
#define SCREAM_CHANNEL_MASK_MONO 0x0004
#define SCREAM_CHANNEL_MASK_STEREO 0x0003
#define SCREAM_CHANNEL_MASK_5_1 0x003F
#define SCREAM_CHANNEL_MASK_7_1 0x00FF

/* Create Scream protocol header */
static void create_scream_header(struct scream_sink_data *data, uint8_t *header)
{
    header[0] = encode_sample_rate(data->format.rate);
    
    /* Sample size in bits */
    switch (data->format.format) {
        case SPA_AUDIO_FORMAT_S16:
            header[1] = 16;
            break;
        case SPA_AUDIO_FORMAT_S24:
            header[1] = 24;
            break;
        case SPA_AUDIO_FORMAT_S32:
        case SPA_AUDIO_FORMAT_F32:
            header[1] = 32;
            break;
        default:
            header[1] = 16;
    }
    
    /* Number of channels */
    header[2] = data->format.channels;
    
    /* Channel mask (WAVEFORMATEXTENSIBLE format, little-endian) */
    uint16_t channel_mask = 0;
    switch (data->format.channels) {
        case 1:
            channel_mask = SCREAM_CHANNEL_MASK_MONO;
            break;
        case 2:
            channel_mask = SCREAM_CHANNEL_MASK_STEREO;
            break;
        case 6:
            channel_mask = SCREAM_CHANNEL_MASK_5_1;
            break;
        case 8:
            channel_mask = SCREAM_CHANNEL_MASK_7_1;
            break;
        default:
            /* Default: sequential channels (clamp to 16 channels max) */
            if (data->format.channels > 16) {
                channel_mask = (1 << 16) - 1;
            } else {
                channel_mask = (1 << data->format.channels) - 1;
            }
            break;
    }
    
    header[3] = channel_mask & 0xFF;
    header[4] = (channel_mask >> 8) & 0xFF;
}

/* Is this buffer entirely zero?
 *
 * Silence is all-zero bytes in every format this module sends - S16/S24/S32 little-endian
 * and F32 alike - so a byte-wise test is both correct and format-agnostic.
 */
static bool buffer_is_silent(const uint8_t *p, size_t n)
{
    size_t i = 0;
    
    /* Word-at-a-time for the bulk; memcpy keeps this well-defined on unaligned input
     * and compiles to a plain load. */
    while (i + sizeof(uint64_t) <= n) {
        uint64_t v;
        memcpy(&v, p + i, sizeof(v));
        if (v != 0) {
            return false;
        }
        i += sizeof(uint64_t);
    }
    for (; i < n; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Send audio packet over network */
static int send_scream_packet(struct scream_sink_data *data, const struct sockaddr_in *dest,
                              const void *audio_data, size_t size)
{
    /* Validate size to prevent buffer overflow */
    if (size > SCREAM_MAX_PAYLOAD) {
        pw_log_error("Packet size %zu exceeds max payload %d", size, SCREAM_MAX_PAYLOAD);
        return -EINVAL;
    }
    
    /* Create header */
    create_scream_header(data, data->packet_buffer);
    
    /* Copy audio data */
    memcpy(data->packet_buffer + SCREAM_HEADER_SIZE, audio_data, size);
    
    /* Send packet */
    ssize_t sent = sendto(data->sockfd, data->packet_buffer, 
                          SCREAM_HEADER_SIZE + size, 0,
                          (const struct sockaddr *)dest, sizeof(*dest));
    
    if (sent < 0) {
        /* Do not log here. This runs in the realtime thread, and if the network is
         * down every single packet fails - one log call per packet, which is both
         * unsafe in an RT context and drowns the log. The caller rate-limits the
         * reporting instead; all we owe it is the errno. */
        return -errno;
    }
    
    return 0;
}

/* Get sample size in bytes from audio format */
static uint32_t get_sample_size(const struct spa_audio_info_raw *format)
{
    switch (format->format) {
        case SPA_AUDIO_FORMAT_S16:
            return 2;
        case SPA_AUDIO_FORMAT_S24:
            return 3;
        case SPA_AUDIO_FORMAT_S32:
        case SPA_AUDIO_FORMAT_F32:
            return 4;
        default:
            return 2;
    }
}

/* Stream process callback - handles audio data from PipeWire */
static void on_stream_process(void *userdata)
{
    struct scream_sink_data *data = userdata;
    struct pw_buffer *buffer;
    struct spa_data *d;
    uint8_t *src;
    uint32_t size;

    buffer = pw_stream_dequeue_buffer(data->stream);
    if (buffer == NULL) {
        return;
    }

    d = buffer->buffer->datas;
    if (d[0].data == NULL || d[0].chunk->size == 0) {
        goto done;
    }

    /* The valid audio starts at chunk->offset, not at the start of the mapped
     * memory. It is zero in the common case, which is exactly why ignoring it
     * is dangerous: nothing breaks until the day it is not, and then we quietly
     * put the wrong bytes on the wire instead of failing. Clamp against the
     * mapped size so a bad offset cannot walk off the end. */
    {
        uint32_t off = SPA_MIN(d[0].chunk->offset, d[0].maxsize);
        size = SPA_MIN(d[0].chunk->size, d[0].maxsize - off);
        if (size == 0) {
            goto done;
        }
        src = SPA_PTROFF(d[0].data, off, uint8_t);
    }

    uint32_t sample_size = get_sample_size(&data->format);
    uint32_t channels = data->format.channels;
    
    /* Prevent integer overflow in bytes_per_frame calculation */
    if (sample_size > 0 && channels > 0) {
        /* Check if multiplication would overflow */
        if (channels > UINT32_MAX / sample_size) {
            pw_log_error("Integer overflow in frame size calculation: sample_size=%u channels=%u",
                        sample_size, channels);
            goto done;
        }
    }
    
    uint32_t bytes_per_frame = sample_size * channels;
    if (bytes_per_frame == 0) {
        pw_log_error("Bytes per frame is zero, aborting processing.");
        goto done;
    }

    /* Silence suppression - do not put silence on the wire.
     *
     * Without this the sink transmits ~1.5 Mbps of zeroes for as long as anything is attached
     * to it, even when every source is quiet. On a battery-powered receiver that is the entire
     * cost of the feature. Upstream Scream solves the same problem with its SilenceThreshold
     * registry value, and this is the same idea with the same name.
     *
     * A receiver cannot distinguish "silence" from "nothing arrived" - the protocol has no
     * keepalive - so receivers already have to treat a gap as silence.
     *
     * The threshold gates *stopping*, never starting: the first non-silent buffer is sent
     * immediately, so no audio is ever clipped at the front of a sound.
     */
    if (data->silence_threshold > 0) {
        if (buffer_is_silent(src, size)) {
            data->silent_frames += size / bytes_per_frame;
            if (data->silent_frames > data->silence_threshold) {
                if (!data->suppressing) {
                    data->suppressing = true;
                    pw_log_info("silence suppression: stopped transmitting after %" PRIu64
                                " silent frames", data->silent_frames);
                }
                goto done;
            }
        } else if (data->suppressing) {
            data->suppressing = false;
            data->silent_frames = 0;
            pw_log_info("silence suppression: resumed transmitting");
        } else {
            data->silent_frames = 0;
        }
    }

    /* One atomic load per buffer: the control protocol may have switched the destination
     * (unicast button). A stale value for one buffer is harmless. */
    struct sockaddr_in dest;
    {
        uint64_t p = atomic_load_explicit(&data->dest_packed, memory_order_relaxed);
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = (uint32_t)(p >> 16);
        dest.sin_port = (uint16_t)(p & 0xFFFF);
    }
    bool sent_any = false;

    uint32_t max_payload_frames = SCREAM_MAX_PAYLOAD / bytes_per_frame;
    uint32_t max_payload_bytes = max_payload_frames * bytes_per_frame;

    uint32_t offset = 0;
    while (offset < size) {
        uint32_t remaining = size - offset;
        uint32_t chunk_size = (remaining > max_payload_bytes) ? 
                              max_payload_bytes : remaining;

        if (chunk_size > 0) {
            int err = send_scream_packet(data, &dest, src + offset, chunk_size);
            if (err == -EAGAIN) {
                /* Send buffer full (EWOULDBLOCK is the same value on Linux).
                 * The packet is gone, but the link is not down - counting this
                 * as a send failure would cry wolf. Report it separately, and
                 * rate-limited, because one busy moment can drop many packets. */
                data->consecutive_send_failures = 0;
                if (++data->dropped_would_block >= MAX_CONSECUTIVE_SEND_FAILURES) {
                    pw_log_warn("Dropped %u packets: send buffer full",
                                data->dropped_would_block);
                    data->dropped_would_block = 0;
                }
            } else if (err < 0) {
                data->consecutive_send_failures++;
                if (data->consecutive_send_failures >= MAX_CONSECUTIVE_SEND_FAILURES) {
                    pw_log_error("Too many consecutive send failures (%u), network may be down: %s",
                                data->consecutive_send_failures, spa_strerror(err));
                    /* Reset counter to avoid log spam */
                    data->consecutive_send_failures = 0;
                }
            } else {
                /* Reset failure counter on success */
                data->consecutive_send_failures = 0;
                sent_any = true;
            }
        }
        offset += chunk_size;
    }
    if (sent_any) {
        /* clock_gettime(CLOCK_MONOTONIC) is a vDSO read - no syscall, safe here */
        atomic_store_explicit(&data->last_send_ns, monotonic_ns(), memory_order_relaxed);
    }

done:
    pw_stream_queue_buffer(data->stream, buffer);
}

/* Stream state changed callback */
static void on_stream_state_changed(void *userdata, enum pw_stream_state old,
                                     enum pw_stream_state state, const char *error)
{
    pw_log_info("Stream state: %s -> %s", 
                pw_stream_state_as_string(old),
                pw_stream_state_as_string(state));
    
    if (state == PW_STREAM_STATE_ERROR) {
        pw_log_error("Stream error: %s", error);
    }
}

/* Stream parameter changed callback */
static void on_stream_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
    struct scream_sink_data *data = userdata;
    
    if (param == NULL || id != SPA_PARAM_Format) {
        return;
    }
    
#ifdef PW_LEGACY_API
    /* PipeWire < 1.0: media_type/media_subtype are in spa_audio_info_raw */
    if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0) {
        return;
    }
    
    if (data->format.media_type != SPA_MEDIA_TYPE_audio ||
        data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
#else
    /* PipeWire >= 1.0: media_type/media_subtype removed, validate separately */
    uint32_t media_type, media_subtype;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0) {
        return;
    }
    
    if (media_type != SPA_MEDIA_TYPE_audio ||
        media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
#endif
    
    spa_format_audio_raw_parse(param, &data->format);
    
    pw_log_info("Format changed: rate=%d channels=%d", 
                data->format.rate,
                data->format.channels);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_stream_process,
};

/* Initialize network socket */
static int init_network(struct scream_sink_data *data)
{
    int ret;
    
    /* Create UDP socket.
     *
     * SOCK_NONBLOCK matters: sendto() is called from the realtime thread. With a
     * blocking socket, a full send buffer stalls that thread, and an xrun there
     * hurts *every* stream in the graph - not just ours. Non-blocking turns the
     * same situation into a dropped packet, which costs only our own audio, and
     * which a Scream receiver already has to cope with: the protocol has no
     * retransmission, so a gap on the wire is a gap either way.
     *
     * Trading a global stall for a local drop is the right way round. */
    data->sockfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (data->sockfd < 0) {
        pw_log_error("Failed to create socket: %s", strerror(errno));
        return -errno;
    }
    
    /* Set up destination address */
    memset(&data->dest_addr, 0, sizeof(data->dest_addr));
    data->dest_addr.sin_family = AF_INET;
    data->dest_addr.sin_port = htons(data->port);
    
    /* Parse IP address */
    ret = inet_pton(AF_INET, data->ip, &data->dest_addr.sin_addr);
    if (ret != 1) {
        pw_log_error("Invalid IP address: %s", data->ip);
        close(data->sockfd);
        return -EINVAL;
    }
    
    /* Enable broadcast for multicast */
    if (IN_MULTICAST(ntohl(data->dest_addr.sin_addr.s_addr))) {
        int broadcast = 1;
        setsockopt(data->sockfd, SOL_SOCKET, SO_BROADCAST, 
                   &broadcast, sizeof(broadcast));
        
        /* Set multicast TTL */
        setsockopt(data->sockfd, IPPROTO_IP, IP_MULTICAST_TTL, 
                   &data->multicast_ttl, sizeof(data->multicast_ttl));
        
        pw_log_info("Multicast TTL set to %u", data->multicast_ttl);
        
        /* Set multicast interface if specified */
        if (data->interface) {
            struct in_addr iface_addr;
            if (inet_pton(AF_INET, data->interface, &iface_addr) == 1) {
                setsockopt(data->sockfd, IPPROTO_IP, IP_MULTICAST_IF,
                           &iface_addr, sizeof(iface_addr));
            } else {
                pw_log_warn("Invalid multicast interface address: %s", data->interface);
            }
        }
    }
    
    pw_log_info("Network initialized: %s:%d", data->ip, data->port);
    return 0;
}

/* Create PipeWire sink stream */
static int create_sink_stream(struct scream_sink_data *data, struct pw_properties *props)
{
    const struct spa_pod *params[1];
    uint8_t buffer[PARAM_BUFFER_SIZE];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    int ret;

    const char *sink_name_str = pw_properties_get(props, "sink.name");
    const char *sink_desc_str = pw_properties_get(props, "sink.description");

    data->sink_name = strdup(sink_name_str ? sink_name_str : DEFAULT_SINK_NAME);
    data->sink_description = strdup(sink_desc_str ? sink_desc_str : DEFAULT_SINK_DESCRIPTION);

    if (!data->sink_name || !data->sink_description) {
        ret = -ENOMEM;
        goto error;
    }

    struct pw_properties *stream_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_NODE_VIRTUAL, "true",
        PW_KEY_NODE_NETWORK, "true",
        NULL
    );

    if (!stream_props) {
        ret = -ENOMEM;
        goto error;
    }

    /* Let the user put arbitrary properties on the node.
     *
     * Everything here used to be a fixed list, so a property the module did not
     * know about had no way to reach the node - node.pause-on-idle and
     * node.latency among them. Callers had no workaround, because the node is
     * created here and nowhere else.
     *
     * The nested form is what the PipeWire modules use (module-loopback has
     * capture.props / playback.props), and it keeps the two namespaces apart:
     * ip and port configure the module, stream.props configures the node.
     * Adding a key is additive - existing configuration keeps working.
     *
     *   args = { ip = "..."  stream.props = { node.pause-on-idle = true } }
     */
    const char *stream_props_str = pw_properties_get(props, "stream.props");
    if (stream_props_str) {
        pw_properties_update_string(stream_props, stream_props_str,
                                    strlen(stream_props_str));
    }

    /* Set last, so they cannot be overridden: the module does not work without
     * this media class, and sink.name is the documented way to name the sink -
     * having two keys that both set it would only be a way to disagree. */
    pw_properties_set(stream_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
    pw_properties_set(stream_props, PW_KEY_NODE_NAME, data->sink_name);
    pw_properties_set(stream_props, PW_KEY_NODE_DESCRIPTION, data->sink_description);

    data->stream = pw_stream_new(data->core, data->sink_name, stream_props);
    if (!data->stream) {
        ret = -ENOMEM;
        goto error;
    }

    pw_stream_add_listener(data->stream, &data->stream_listener, &stream_events, data);

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SCREAM_AUDIO_FORMAT,
            .rate = data->format.rate,
            .channels = data->format.channels
        ));

    ret = pw_stream_connect(data->stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS |
        PW_STREAM_FLAG_AUTOCONNECT,
        params, 1);

    if (ret < 0) {
        goto error;
    }

    pw_log_info("Created sink stream: %s", data->sink_name);
    return 0;

error:
    pw_log_error("Failed to create sink stream: %s", spa_strerror(ret));
    if (data->stream) {
        pw_stream_destroy(data->stream);
        data->stream = NULL;
    }
    free(data->sink_name);
    data->sink_name = NULL;
    free(data->sink_description);
    data->sink_description = NULL;
    return ret;
}

/* ------------------------------------------------------------------------------------------
 * Control protocol: unicast button and auto output (TECHNICAL.md "Control protocol").
 *
 * Everything in this section runs on the main loop. The only thing the RT thread sees is
 * dest_packed (one atomic word) and last_send_ns.
 * ------------------------------------------------------------------------------------------ */

static void set_dest(struct scream_sink_data *data, const struct sockaddr_in *a)
{
    atomic_store_explicit(&data->dest_packed, pack_dest(a), memory_order_relaxed);
}

static const char *addr_str(const struct sockaddr_in *a, char *buf, size_t len)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(buf, len, "%s:%u", ip, ntohs(a->sin_port));
    return buf;
}

/* Copy at most max bytes of a UTF-8 string, never cutting a character in half. */
static void utf8_copy(char *dst, const char *src, size_t max)
{
    size_t n = strlen(src);
    if (n > max) {
        n = max;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool ctrl_allowed(struct scream_sink_data *data, const struct sockaddr_in *from)
{
    if (data->n_allow == 0) {
        return true;
    }
    uint32_t a = ntohl(from->sin_addr.s_addr);
    for (int i = 0; i < data->n_allow; i++) {
        if ((a & data->allow[i].mask) == data->allow[i].net) {
            return true;
        }
    }
    return false;
}

/* "192.168.144.0/24, 10.0.0.0/8" - separators: space, comma, semicolon */
static int parse_allow(struct scream_sink_data *data, const char *str)
{
    char buf[512];
    utf8_copy(buf, str, sizeof(buf) - 1);
    char *save = NULL;
    for (char *tok = strtok_r(buf, " ,;", &save); tok; tok = strtok_r(NULL, " ,;", &save)) {
        if (data->n_allow >= CTRL_MAX_ALLOW) {
            pw_log_error("discovery.allow: more than %d entries", CTRL_MAX_ALLOW);
            return -EINVAL;
        }
        long bits = 32;
        char *slash = strchr(tok, '/');
        if (slash) {
            *slash = '\0';
            if (parse_int_param(slash + 1, "discovery.allow prefix", 0, 32, &bits) < 0) {
                return -EINVAL;
            }
        }
        struct in_addr in;
        if (inet_pton(AF_INET, tok, &in) != 1) {
            pw_log_error("discovery.allow: invalid address %s", tok);
            return -EINVAL;
        }
        uint32_t mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
        data->allow[data->n_allow].mask = mask;
        data->allow[data->n_allow].net = ntohl(in.s_addr) & mask;
        data->n_allow++;
    }
    return 0;
}

static void ctrl_send(struct scream_sink_data *data, const struct sockaddr_in *to,
                      const uint8_t *buf, size_t len)
{
    /* Non-blocking socket: a lost answer is covered by the phone's retry. */
    if (sendto(data->ctrl_fd, buf, len, 0, (const struct sockaddr *)to, sizeof(*to)) < 0) {
        char a[32];
        pw_log_debug("control: send to %s failed: %s", addr_str(to, a, sizeof(a)),
                     strerror(errno));
    }
}

static size_t put_header(uint8_t *p, uint8_t type)
{
    memcpy(p, CTRL_MAGIC, CTRL_MAGIC_LEN);
    p[8] = type;
    return 9;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t put_name(uint8_t *p, const char *name)
{
    size_t n = strlen(name);
    p[0] = (uint8_t)n;
    memcpy(p + 1, name, n);
    return 1 + n;
}

static void ctrl_answer(struct scream_sink_data *data, const struct sockaddr_in *to,
                        uint32_t request_id)
{
    uint8_t p[CTRL_MAX_PACKET];
    size_t n = put_header(p, CTRL_ANSWER);
    put_u32(p + n, request_id); n += 4;
    put_u32(p + n, data->sender_id); n += 4;

    uint8_t mode = 0;
    if (data->uni_active) {
        mode = data->uni_ctrl.sin_addr.s_addr == to->sin_addr.s_addr ? 1 : 2;
    }
    p[n++] = mode;

    uint64_t last = atomic_load_explicit(&data->last_send_ns, memory_order_relaxed);
    p[n++] = (last != 0 && monotonic_ns() - last < CTRL_SENDING_WINDOW_NS) ? 1 : 0;

    /* Right after we wrote the default, WirePlumber has not confirmed it yet: report
     * what we asked for, so the answer to a connect already says "default". */
    bool is_def = data->default_expect >= 0 ? data->default_expect : data->is_default;
    p[n++] = !data->auto_output ? 0 : (is_def ? 2 : 1);
    n += put_name(p + n, data->nickname);
    ctrl_send(data, to, p, n);
}

static void ctrl_notice(struct scream_sink_data *data, const struct sockaddr_in *to,
                        uint8_t reason)
{
    uint8_t p[CTRL_MAX_PACKET];
    size_t n = put_header(p, CTRL_NOTICE);
    put_u32(p + n, data->sender_id); n += 4;
    p[n++] = reason;
    n += put_name(p + n, data->nickname);
    ctrl_send(data, to, p, n);
}

static void uni_stop(struct scream_sink_data *data, uint8_t notice_reason)
{
    if (!data->uni_active) {
        return;
    }
    char a[32];
    data->uni_active = false;
    set_dest(data, &data->dest_addr);
    pw_log_info("control: back to %s:%d (was unicast to %s)", data->ip, data->port,
                addr_str(&data->uni_ctrl, a, sizeof(a)));
    if (notice_reason) {
        ctrl_notice(data, &data->uni_ctrl, notice_reason);
    }
}

/* ---- default output (PipeWire "default" metadata) ---- */

/* WirePlumber stores {"name":"<node.name>"}; take the name, "" if absent. */
static void parse_default_name(const char *value, char *out, size_t len)
{
    struct spa_json it[2];
    char key[64];
    const char *v;

    out[0] = '\0';
    if (value == NULL) {
        return;
    }
    spa_json_init(&it[0], value, strlen(value));
    if (spa_json_enter_object(&it[0], &it[1]) <= 0) {
        return;
    }
    while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
        if (spa_streq(key, "name")) {
            if (spa_json_get_string(&it[1], out, len) <= 0) {
                out[0] = '\0';
            }
            return;
        }
        if (spa_json_next(&it[1], &v) <= 0) {
            return;
        }
    }
}

static void set_default_sink(struct scream_sink_data *data, const char *name)
{
    char json[NODE_NAME_MAX * 2 + 16];
    size_t j = 0;

    if (data->metadata == NULL) {
        pw_log_warn("control: no default metadata yet, cannot set the default output");
        return;
    }
    j += snprintf(json, sizeof(json), "{\"name\":\"");
    for (const char *s = name; *s && j < sizeof(json) - 4; s++) {
        if (*s == '"' || *s == '\\') {
            json[j++] = '\\';
        }
        json[j++] = *s;
    }
    json[j++] = '"';
    json[j++] = '}';
    json[j] = '\0';
    pw_metadata_set_property(data->metadata, PW_ID_CORE, "default.configured.audio.sink",
                             "Spa:String:JSON", json);
    pw_log_info("control: default output -> %s", name);
}

static int on_metadata_property(void *userdata, uint32_t subject, const char *key,
                                const char *type, const char *value)
{
    struct scream_sink_data *data = userdata;

    if (subject != PW_ID_CORE) {
        return 0;
    }
    if (key == NULL) {
        /* everything cleared */
        data->configured_sink[0] = '\0';
        data->effective_sink[0] = '\0';
        data->default_expect = -1;
    } else if (spa_streq(key, "default.configured.audio.sink")) {
        parse_default_name(value, data->configured_sink, sizeof(data->configured_sink));
        return 0;
    } else if (spa_streq(key, "default.audio.sink")) {
        parse_default_name(value, data->effective_sink, sizeof(data->effective_sink));
        data->default_expect = -1;
    } else {
        return 0;
    }

    bool now = spa_streq(data->effective_sink, data->sink_name);
    bool was = data->is_default;
    data->is_default = now;
    if (was && !now) {
        pw_log_info("control: %s is no longer the default output (now %s)", data->sink_name,
                    data->effective_sink[0] ? data->effective_sink : "none");
        /* Someone else moved it while phones are connected: their choice wins. */
        if (data->n_connected > 0 && data->remembered_sink[0]) {
            pw_log_info("control: output moved by hand; will not restore %s",
                        data->remembered_sink);
            data->remembered_sink[0] = '\0';
        }
        uni_stop(data, CTRL_NOTICE_OUTPUT_MOVED);
    }
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_metadata_property,
};

static void unbind_metadata(struct scream_sink_data *data)
{
    if (data->metadata) {
        spa_hook_remove(&data->metadata_listener);
        pw_proxy_destroy((struct pw_proxy *)data->metadata);
        data->metadata = NULL;
    }
}

static void on_registry_global(void *userdata, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version, const struct spa_dict *props)
{
    struct scream_sink_data *data = userdata;
    const char *name;

    if (!spa_streq(type, PW_TYPE_INTERFACE_Metadata) || props == NULL) {
        return;
    }
    name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
    if (!spa_streq(name, "default")) {
        return;
    }
    unbind_metadata(data);
    data->metadata = pw_registry_bind(data->registry, id, PW_TYPE_INTERFACE_Metadata,
                                      PW_VERSION_METADATA, 0);
    if (data->metadata == NULL) {
        pw_log_warn("control: cannot bind the default metadata");
        return;
    }
    data->metadata_id = id;
    pw_metadata_add_listener(data->metadata, &data->metadata_listener, &metadata_events, data);
    pw_log_info("control: watching the default output");
}

static void on_registry_global_remove(void *userdata, uint32_t id)
{
    struct scream_sink_data *data = userdata;
    /* WirePlumber restarting: drop it; a new global brings it back */
    if (data->metadata && id == data->metadata_id) {
        unbind_metadata(data);
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

/* ---- auto output ---- */

static int connected_find(struct scream_sink_data *data, uint32_t instance)
{
    for (int i = 0; i < data->n_connected; i++) {
        if (data->connected[i].instance == instance) {
            return i;
        }
    }
    return -1;
}

static void auto_connect(struct scream_sink_data *data, uint32_t instance,
                         const struct sockaddr_in *from, const char *dev)
{
    char a[32];
    int i = connected_find(data, instance);

    if (i >= 0) {
        data->connected[i].addr = *from;     /* repeat, or the address changed */
        return;
    }
    if (data->n_connected >= CTRL_MAX_CONNECTED) {
        pw_log_warn("control: %d phones connected already, ignoring %s", CTRL_MAX_CONNECTED,
                    addr_str(from, a, sizeof(a)));
        return;
    }
    bool first = data->n_connected == 0;
    data->connected[data->n_connected].instance = instance;
    data->connected[data->n_connected].addr = *from;
    data->n_connected++;
    pw_log_info("control: connected %s (%s), %d now", addr_str(from, a, sizeof(a)), dev,
                data->n_connected);

    if (first && !data->is_default) {
        const char *prev = data->configured_sink[0] ? data->configured_sink
                                                    : data->effective_sink;
        snprintf(data->remembered_sink, sizeof(data->remembered_sink), "%s", prev);
        pw_log_info("control: remembering %s", prev[0] ? prev : "(none)");
        set_default_sink(data, data->sink_name);
        data->default_expect = 1;
    }
}

static void auto_disconnect(struct scream_sink_data *data, uint32_t instance,
                            const struct sockaddr_in *from)
{
    char a[32];
    int i = connected_find(data, instance);

    if (i < 0) {
        return;                              /* repeat of a disconnect we already handled */
    }
    data->connected[i] = data->connected[--data->n_connected];
    pw_log_info("control: disconnected %s, %d left", addr_str(from, a, sizeof(a)),
                data->n_connected);
    if (data->n_connected > 0) {
        return;
    }
    if (data->remembered_sink[0] && data->is_default) {
        /* If that device is gone, WirePlumber falls back to its own choice. */
        set_default_sink(data, data->remembered_sink);
        data->default_expect = 0;
    }
    data->remembered_sink[0] = '\0';
}

/* ---- the socket ---- */

static bool read_name(const uint8_t *p, size_t len, size_t at, char *out, size_t outlen)
{
    if (at >= len) {
        return false;
    }
    size_t n = p[at];
    if (n > CTRL_NAME_MAX || at + 1 + n > len || n >= outlen) {
        return false;
    }
    memcpy(out, p + at + 1, n);
    out[n] = '\0';
    /* for the log only: keep it printable */
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)out[i] < 0x20) {
            out[i] = '?';
        }
    }
    return true;
}

static void on_ctrl_io(void *userdata, int fd, uint32_t mask)
{
    struct scream_sink_data *data = userdata;
    uint8_t p[256];
    struct sockaddr_in from;
    socklen_t fl;
    ssize_t len;
    char a[32], dev[CTRL_NAME_MAX + 1];

    /* drain everything that is queued */
    for (;;) {
        fl = sizeof(from);
        len = recvfrom(fd, p, sizeof(p), 0, (struct sockaddr *)&from, &fl);
        if (len < 0) {
            return;                          /* EAGAIN: done */
        }
        if (fl != sizeof(from) || from.sin_family != AF_INET) {
            continue;
        }
        if (len < 13 || memcmp(p, CTRL_MAGIC, CTRL_MAGIC_LEN) != 0) {
            continue;
        }
        if (!ctrl_allowed(data, &from)) {
            continue;                        /* no answer reveals nothing */
        }
        uint8_t type = p[8];
        uint32_t req = get_u32(p + 9);

        switch (type) {
        case CTRL_QUERY:
            ctrl_answer(data, &from, req);
            break;

        case CTRL_TAKE: {
            if (len < 16 || !read_name(p, len, 15, dev, sizeof(dev))) {
                break;
            }
            uint16_t audio_port = (uint16_t)(p[13] | p[14] << 8);
            if (audio_port == 0) {
                break;
            }
            if (data->uni_active && data->uni_ctrl.sin_addr.s_addr != from.sin_addr.s_addr) {
                char b[32];
                pw_log_info("control: %s (%s) takes over from %s", addr_str(&from, a, sizeof(a)),
                            dev, addr_str(&data->uni_ctrl, b, sizeof(b)));
                ctrl_notice(data, &data->uni_ctrl, CTRL_NOTICE_TAKEN);
            }
            data->uni_active = true;
            data->uni_ctrl = from;
            data->uni_audio_port = audio_port;
            struct sockaddr_in d = from;
            d.sin_port = htons(audio_port);
            set_dest(data, &d);
            pw_log_info("control: unicast to %s (%s)", addr_str(&d, a, sizeof(a)), dev);
            ctrl_answer(data, &from, req);
            break;
        }

        case CTRL_RELEASE:
            if (data->uni_active && data->uni_ctrl.sin_addr.s_addr == from.sin_addr.s_addr) {
                uni_stop(data, 0);
            }
            ctrl_answer(data, &from, req);
            break;

        case CTRL_CONNECT:
            if (len < 18 || !read_name(p, len, 17, dev, sizeof(dev))) {
                break;
            }
            if (data->auto_output) {
                auto_connect(data, get_u32(p + 13), &from, dev);
            }
            ctrl_answer(data, &from, req);
            break;

        case CTRL_DISCONNECT:
            if (len < 17) {
                break;
            }
            if (data->auto_output) {
                auto_disconnect(data, get_u32(p + 13), &from);
            }
            ctrl_answer(data, &from, req);
            break;

        default:                             /* keepalive (reserved) and unknown types */
            break;
        }
    }
}

static void ctrl_start(struct scream_sink_data *data)
{
    struct sockaddr_in a;

    data->ctrl_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (data->ctrl_fd < 0) {
        pw_log_warn("control: socket failed (%s); feature off", strerror(errno));
        return;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(data->ctrl_port);
    /* No SO_REUSEADDR on purpose: a second instance on this host must fail here and run
     * with the feature off, instead of two senders both answering and both sending. */
    if (bind(data->ctrl_fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        pw_log_warn("control: cannot bind UDP %d (%s) - another instance has it; feature off",
                    data->ctrl_port, strerror(errno));
        close(data->ctrl_fd);
        data->ctrl_fd = -1;
        return;
    }
    data->ctrl_source = pw_loop_add_io(pw_context_get_main_loop(data->context), data->ctrl_fd,
                                       SPA_IO_IN, false, on_ctrl_io, data);

    data->registry = pw_core_get_registry(data->core, PW_VERSION_REGISTRY, 0);
    if (data->registry) {
        pw_registry_add_listener(data->registry, &data->registry_listener, &registry_events,
                                 data);
    }
    pw_log_info("control: listening on UDP %d as \"%s\"%s", data->ctrl_port, data->nickname,
                data->auto_output ? ", auto output on" : "");
}

static void ctrl_stop(struct scream_sink_data *data)
{
    unbind_metadata(data);
    if (data->registry) {
        spa_hook_remove(&data->registry_listener);
        pw_proxy_destroy((struct pw_proxy *)data->registry);
        data->registry = NULL;
    }
    if (data->ctrl_source) {
        pw_loop_destroy_source(pw_context_get_main_loop(data->context), data->ctrl_source);
        data->ctrl_source = NULL;
    }
    if (data->ctrl_fd >= 0) {
        close(data->ctrl_fd);
        data->ctrl_fd = -1;
    }
}

/* Core events */
static void on_core_error(void *userdata, uint32_t id, int seq, int res, const char *message)
{
    pw_log_error("Core error: id=%u seq=%d res=%d: %s", id, seq, res, message);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

/* Module cleanup */
static void module_destroy(void *data)
{
    struct scream_sink_data *d = data;
    
    pw_log_info("Module destroy called");

    ctrl_stop(d);
    
    /* Clean up socket */
    if (d->sockfd >= 0) {
        pw_log_info("Closing socket...");
        close(d->sockfd);
        d->sockfd = -1;
    }
    
    /* Free allocated strings */
    pw_log_info("Freeing memory...");
    free(d->ip);
    d->ip = NULL;
    free(d->interface);
    d->interface = NULL;
    free(d->sink_name);
    d->sink_name = NULL;
    free(d->sink_description);
    d->sink_description = NULL;
    
    pw_log_info("Module destroy complete");
}

static const struct pw_impl_module_events module_events = {
    PW_VERSION_IMPL_MODULE_EVENTS,
    .destroy = module_destroy,
};

/* Module initialization */
SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
    struct pw_context *context = pw_impl_module_get_context(module);
    struct scream_sink_data *data;
    struct pw_properties *props = NULL;
    const char *str;
    int ret = 0;

    pw_log_info("Scream module init starting...");

    data = calloc(1, sizeof(struct scream_sink_data));
    if (!data) {
        pw_log_error("Failed to allocate module data");
        return -ENOMEM;
    }

    data->module = module;
    data->sockfd = -1;
    data->multicast_ttl = DEFAULT_MULTICAST_TTL;
    data->consecutive_send_failures = 0;
    data->dropped_would_block = 0;
    data->ctrl_fd = -1;
    data->default_expect = -1;

    props = args ? pw_properties_new_string(args) : pw_properties_new(NULL, NULL);
    if (!props) {
        ret = -ENOMEM;
        goto error;
    }

    str = pw_properties_get(props, "ip");
    data->ip = strdup(str ? str : SCREAM_MULTICAST_GROUP);
    if (!data->ip) {
        ret = -ENOMEM;
        goto error;
    }

    str = pw_properties_get(props, "port");
    if (str) {
        long port;
        ret = parse_int_param(str, "port", MIN_PORT, MAX_PORT, &port);
        if (ret < 0) {
            goto error;
        }
        data->port = (int)port;
    } else {
        data->port = SCREAM_DEFAULT_PORT;
    }

    str = pw_properties_get(props, "interface");
    if (str) {
        data->interface = strdup(str);
        if (!data->interface) {
            ret = -ENOMEM;
            goto error;
        }
    }
    
    str = pw_properties_get(props, "multicast.ttl");
    if (str) {
        long ttl;
        ret = parse_int_param(str, "multicast.ttl", 1, MAX_MULTICAST_TTL, &ttl);
        if (ret < 0) {
            goto error;
        }
        data->multicast_ttl = (unsigned char)ttl;
    }

    if ((ret = init_network(data)) < 0) {
        goto error;
    }

    str = pw_properties_get(props, "rate");
    if (str) {
        long rate;
        ret = parse_int_param(str, "sample rate", MIN_SAMPLE_RATE, MAX_SAMPLE_RATE, &rate);
        if (ret < 0) {
            goto error;
        }
        data->format.rate = (uint32_t)rate;
    } else {
        data->format.rate = 48000;
    }

    str = pw_properties_get(props, "channels");
    if (str) {
        long channels;
        ret = parse_int_param(str, "channels", MIN_CHANNELS, MAX_CHANNELS, &channels);
        if (ret < 0) {
            goto error;
        }
        data->format.channels = (uint32_t)channels;
    } else {
        data->format.channels = 2;
    }
    
    str = pw_properties_get(props, "format");
    if (str) {
        if (strcmp(str, "S16LE") == 0) {
            data->format.format = SPA_AUDIO_FORMAT_S16;
        } else if (strcmp(str, "S24LE") == 0) {
            data->format.format = SPA_AUDIO_FORMAT_S24;
        } else if (strcmp(str, "S32LE") == 0) {
            data->format.format = SPA_AUDIO_FORMAT_S32;
        } else {
            pw_log_error("Invalid audio format: %s (supported: S16LE, S24LE, S32LE)", str);
            ret = -EINVAL;
            goto error;
        }
    } else {
        data->format.format = SPA_AUDIO_FORMAT_S16;
    }

    str = pw_properties_get(props, "silence.threshold");
    if (str) {
        long frames;
        ret = parse_int_param(str, "silence threshold", 0, MAX_SILENCE_FRAMES, &frames);
        if (ret < 0) {
            goto error;
        }
        data->silence_threshold = (uint64_t)frames;
    } else {
        data->silence_threshold = DEFAULT_SILENCE_FRAMES;
    }

    set_dest(data, &data->dest_addr);

    str = pw_properties_get(props, "discovery.port");
    if (str) {
        long cport;
        ret = parse_int_param(str, "discovery.port", 0, MAX_PORT, &cport);
        if (ret < 0) {
            goto error;
        }
        data->ctrl_port = (int)cport;
    }
    str = pw_properties_get(props, "discovery.name");
    if (str && *str) {
        utf8_copy(data->nickname, str, CTRL_NAME_MAX);
    } else {
        char host[256] = "";
        gethostname(host, sizeof(host) - 1);
        utf8_copy(data->nickname, host, CTRL_NAME_MAX);
    }
    str = pw_properties_get(props, "discovery.auto.output");
    data->auto_output = str && spa_atob(str);
    str = pw_properties_get(props, "discovery.allow");
    if (str && (ret = parse_allow(data, str)) < 0) {
        goto error;
    }
    {
        /* any value works; it only has to differ between module loads */
        uint64_t t = monotonic_ns() ^ ((uint64_t)getpid() << 32);
        data->sender_id = (uint32_t)(t ^ (t >> 32));
    }

    data->context = context;
    data->core = pw_context_connect(context, NULL, 0);
    if (!data->core) {
        ret = -errno;
        goto error;
    }

    pw_core_add_listener(data->core, &data->core_listener, &core_events, data);

    if ((ret = create_sink_stream(data, props)) < 0) {
        goto error;
    }

    if (data->ctrl_port > 0) {
        ctrl_start(data);
    }

    pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);
    pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

    pw_log_info("Scream sender module loaded: %s:%d", data->ip, data->port);

    pw_properties_free(props);
    return 0;

error:
    pw_log_error("Module init failed with error: %d", ret);
    if (data) {
        if (data->core) {
            pw_core_disconnect(data->core);
        }
        if (data->sockfd >= 0) {
            close(data->sockfd);
        }
        free(data->ip);
        free(data->interface);
        free(data->sink_name);
        free(data->sink_description);
        free(data);
    }
    if (props) {
        pw_properties_free(props);
    }
    return ret;
}
