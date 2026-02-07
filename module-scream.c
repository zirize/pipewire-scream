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
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MODULE_NAME "module-scream"
#define DEFAULT_SINK_NAME "Scream"
#define DEFAULT_SINK_DESCRIPTION "Scream Network Sink"

/* Scream protocol constants */
#define SCREAM_MULTICAST_GROUP "239.255.77.77"
#define SCREAM_DEFAULT_PORT 4010
#define SCREAM_HEADER_SIZE 5
#define SCREAM_MAX_PAYLOAD 1152

/* Sample rate encoding for Scream protocol */
#define RATE_BASE_48K 0
#define RATE_BASE_44K 1

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
    
    /* Sink properties */
    char *sink_name;
    char *sink_description;
    
    /* Audio format */
    struct spa_audio_info_raw format;
    
    /* Scream protocol state */
    uint8_t packet_buffer[SCREAM_HEADER_SIZE + SCREAM_MAX_PAYLOAD];
};

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

/* Send audio packet over network */
static int send_scream_packet(struct scream_sink_data *data, const void *audio_data, size_t size)
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
                          (struct sockaddr *)&data->dest_addr, 
                          sizeof(data->dest_addr));
    
    if (sent < 0) {
        pw_log_error("Failed to send packet: %s", strerror(errno));
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

    src = d[0].data;
    size = d[0].chunk->size;

    uint32_t sample_size = get_sample_size(&data->format);
    uint32_t bytes_per_frame = sample_size * data->format.channels;
    if (bytes_per_frame == 0) {
        pw_log_error("Bytes per frame is zero, aborting processing.");
        goto done;
    }

    uint32_t max_payload_frames = SCREAM_MAX_PAYLOAD / bytes_per_frame;
    uint32_t max_payload_bytes = max_payload_frames * bytes_per_frame;

    uint32_t offset = 0;
    while (offset < size) {
        uint32_t remaining = size - offset;
        uint32_t chunk_size = (remaining > max_payload_bytes) ? 
                              max_payload_bytes : remaining;

        if (chunk_size > 0 && send_scream_packet(data, src + offset, chunk_size) < 0) {
            pw_log_debug("Failed to send packet, offset=%u size=%u", offset, size);
        }
        offset += chunk_size;
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
    
    /* Create UDP socket */
    data->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
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
        unsigned char ttl = 1;
        setsockopt(data->sockfd, IPPROTO_IP, IP_MULTICAST_TTL, 
                   &ttl, sizeof(ttl));
        
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
    uint8_t buffer[1024];
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
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_NAME, data->sink_name,
        PW_KEY_NODE_DESCRIPTION, data->sink_description,
        PW_KEY_NODE_VIRTUAL, "true",
        PW_KEY_NODE_NETWORK, "true",
        NULL
    );

    if (!stream_props) {
        ret = -ENOMEM;
        goto error;
    }

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
    
    /* Note: PipeWire automatically cleans up all PipeWire objects (stream, core, listeners)
     * when the context is destroyed. We MUST NOT touch them here.
     * Only clean up our own resources: socket and memory.
     */
    
    if (d->sockfd >= 0) {
        pw_log_info("Closing socket...");
        close(d->sockfd);
    }
    
    pw_log_info("Freeing memory...");
    free(d->ip);
    free(d->interface);
    free(d->sink_name);
    free(d->sink_description);
    
    /* Do NOT free(d) here! PipeWire may still need to access the module data
     * after this callback returns. Let it manage the lifecycle.
     */
    
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
        char *endptr;
        long port = strtol(str, &endptr, 10);
        if (*endptr != '\0' || port < 1 || port > 65535) {
            pw_log_error("Invalid port number: %s", str);
            ret = -EINVAL;
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

    if ((ret = init_network(data)) < 0) {
        goto error;
    }

    str = pw_properties_get(props, "rate");
    if (str) {
        char *endptr;
        long rate = strtol(str, &endptr, 10);
        if (*endptr != '\0' || rate < 8000 || rate > 384000) {
            pw_log_error("Invalid sample rate: %s", str);
            ret = -EINVAL;
            goto error;
        }
        data->format.rate = (uint32_t)rate;
    } else {
        data->format.rate = 48000;
    }

    str = pw_properties_get(props, "channels");
    if (str) {
        char *endptr;
        long channels = strtol(str, &endptr, 10);
        if (*endptr != '\0' || channels < 1 || channels > 255) {
            pw_log_error("Invalid channel count: %s", str);
            ret = -EINVAL;
            goto error;
        }
        data->format.channels = (uint32_t)channels;
    } else {
        data->format.channels = 2;
    }
    
    str = pw_properties_get(props, "format");
    if (str && strcmp(str, "S24LE") == 0) {
        data->format.format = SPA_AUDIO_FORMAT_S24;
    } else if (str && strcmp(str, "S32LE") == 0) {
        data->format.format = SPA_AUDIO_FORMAT_S32;
    } else {
        data->format.format = SPA_AUDIO_FORMAT_S16;
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
