#define _GNU_SOURCE // memfd_create
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <gst/video/video.h>

#include "cflogprinter.h"
#include "playlist.h"
#include "slideshow.h"

// How long before an item's turn its successor starts loading. Copying a
// large video into memory and prerolling it must finish within this time
// for the switch to happen on schedule; if it does not, the current item
// keeps playing until the next one is ready.
#define SLIDESHOW_PRELOAD_LEAD_SECS 15.0
// Wait between folder rescans while nothing in it can be shown.
#define SLIDESHOW_RETRY_SECS 5.0
// Upper bound for decoding an item's first frame.
#define SLOT_PREROLL_TIMEOUT_US (30 * G_USEC_PER_SEC)
#define SLOT_COPY_CHUNK (1024 * 1024)

// playbin "flags" bits
#define PLAY_FLAG_VIDEO       (1 << 0)
#define PLAY_FLAG_AUDIO       (1 << 1)
#define PLAY_FLAG_SOFT_VOLUME (1 << 4)

typedef enum {
    SLOT_PREPARING, // the slot thread is loading and prerolling
    SLOT_READY,     // first frame decoded; the pipeline is paused at the start
    SLOT_FAILED,    // could not be loaded or hit an error while playing
} slot_status;

struct media_slot {
    uint64_t id;
    char *name; // file name inside the folder
    char *path;
    bool still; // single image: nothing to loop
    bool gif;   // animated GIF: needs the delayed flushing loop below
    const slideshow_config *config;

    pthread_t thread;
    bool thread_started;
    atomic_int status;
    atomic_bool stopping;

    // Written by the slot thread before it reports SLOT_READY, read-only
    // afterwards, so the main thread may use them once it sees SLOT_READY.
    int memfd; // -1 when streaming from disk
    GstElement *pipeline;
    GstBus *bus;
    GstPad *probe_pad; // appsink pad carrying slot_frame_probe
    gulong probe_id;

    // Only touched by the slot thread
    int64_t loop_start_us;

    // Latest decoded frame, handed from the streaming thread to the renderer
    pthread_mutex_t frame_lock;
    GstBuffer *frame;
    int frame_width, frame_height;
    bool frame_new;

    // Only touched by the streaming thread
    GstCaps *caps;
    int caps_width, caps_height;
};

// A small set of file names; folders hold few enough files for a list.
typedef struct {
    char **names;
    size_t count;
} name_set;

struct slideshow {
    slideshow_config config;
    media_slot *current; // on screen; NULL until the first item is ready
    media_slot *next;    // loading, ready, or fading in when `fading`
    bool fading;
    double current_shown; // unpaused seconds since current began showing
    double fade_elapsed;
    int64_t last_tick_us;
    bool paused;
    int64_t retry_at_us;
    name_set failed; // names that failed since the last successful switch
    name_set played; // with shuffle: names shown in the current round
    uint64_t next_slot_id;
    bool warned_empty;
};

// Asynchronous slot teardowns still running; slideshow_free waits for them.
static atomic_int teardowns_in_flight = 0;

// ==== Media slots ====

static void wake_main_loop(const media_slot *slot) {
    if (write(slot->config->wakeup_fd, "s", 1) == -1 && errno != EAGAIN && slot->config->verbose)
        cflp_warning("Slideshow: failed to write to wakeup pipe");
}

static GstPadProbeReturn slot_frame_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    media_slot *slot = data;
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer)
        return GST_PAD_PROBE_OK;

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps)
        return GST_PAD_PROBE_OK;
    if (caps != slot->caps) {
        GstVideoInfo video_info;
        bool rgba = gst_video_info_from_caps(&video_info, caps) &&
                    GST_VIDEO_INFO_FORMAT(&video_info) == GST_VIDEO_FORMAT_RGBA;
        slot->caps_width = rgba ? GST_VIDEO_INFO_WIDTH(&video_info) : 0;
        slot->caps_height = rgba ? GST_VIDEO_INFO_HEIGHT(&video_info) : 0;
        if (slot->caps)
            gst_caps_unref(slot->caps);
        slot->caps = caps; // keep this reference as the cache
    } else {
        gst_caps_unref(caps);
    }

    int width = slot->caps_width, height = slot->caps_height;
    if (width <= 0 || height <= 0 || gst_buffer_get_size(buffer) < (gsize)width * height * 4)
        return GST_PAD_PROBE_OK;

    pthread_mutex_lock(&slot->frame_lock);
    if (slot->frame)
        gst_buffer_unref(slot->frame);
    slot->frame = gst_buffer_ref(buffer);
    slot->frame_width = width;
    slot->frame_height = height;
    slot->frame_new = true;
    pthread_mutex_unlock(&slot->frame_lock);

    wake_main_loop(slot);
    return GST_PAD_PROBE_OK;
}

static bool slot_has_frame(media_slot *slot) {
    pthread_mutex_lock(&slot->frame_lock);
    bool has_frame = slot->frame != NULL;
    pthread_mutex_unlock(&slot->frame_lock);
    return has_frame;
}

// Copies src into dst. Fails when a read or write fails, when stopping is
// raised, or when more than limit bytes arrive (the file grew past it).
static bool copy_file_contents(int src, int dst, size_t limit, const atomic_bool *stopping) {
    char *buffer = malloc(SLOT_COPY_CHUNK);
    if (!buffer)
        return false;
    size_t total = 0;
    bool ok = true;
    while (ok && !atomic_load(stopping)) {
        ssize_t n = read(src, buffer, SLOT_COPY_CHUNK);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        total += (size_t)n;
        if (total > limit) {
            ok = false;
            break;
        }
        for (ssize_t written = 0; ok && written < n;) {
            ssize_t w = write(dst, buffer + written, (size_t)(n - written));
            if (w < 0 && errno == EINTR)
                continue;
            ok = w > 0;
            written += w > 0 ? w : 0;
        }
    }
    free(buffer);
    return ok && !atomic_load(stopping);
}

// Copies the file into an anonymous in-memory file when it fits within the
// preload limit, so playback never waits on the disk and keeps working if
// the original is deleted. Larger files are streamed, with the kernel asked
// to start reading them into its cache now.
static bool slot_load_source(media_slot *slot) {
    int fd = open(slot->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        cflp_warning("Slideshow: cannot open %s: %s", slot->path, strerror(errno));
        return false;
    }
    struct stat st;
    size_t limit = slot->config->preload_limit_bytes;
    if (fstat(fd, &st) == 0 && limit > 0 && (size_t)st.st_size <= limit) {
        int memfd = memfd_create("gslapper-slide", MFD_CLOEXEC);
        if (memfd >= 0 && copy_file_contents(fd, memfd, limit, &slot->stopping)) {
            slot->memfd = memfd;
        } else if (memfd >= 0) {
            close(memfd);
        }
    }
    if (slot->memfd < 0)
        posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
    if (slot->config->verbose)
        cflp_info("Slideshow: %s %s", slot->memfd >= 0 ? "loaded into memory:" : "streaming from disk:", slot->name);
    close(fd);
    return !atomic_load(&slot->stopping);
}

static char *slot_uri(const media_slot *slot) {
    if (slot->memfd >= 0)
        return g_strdup_printf("file:///proc/self/fd/%d", slot->memfd);
    return gst_filename_to_uri(slot->path, NULL);
}

static bool slot_build_pipeline(media_slot *slot) {
    char *uri = slot_uri(slot);
    GstElement *playbin = gst_element_factory_make("playbin", NULL);
    GstElement *appsink = gst_element_factory_make("appsink", NULL);
    if (!uri || !playbin || !appsink) {
        cflp_warning("Slideshow: cannot create a pipeline for %s", slot->name);
        g_free(uri);
        if (playbin)
            gst_object_unref(playbin);
        if (appsink)
            gst_object_unref(appsink);
        return false;
    }

    GstCaps *caps = gst_caps_from_string("video/x-raw,format=RGBA");
    g_object_set(appsink, "caps", caps, "sync", TRUE, "drop", TRUE, "max-buffers", 1, NULL);
    gst_caps_unref(caps);
    slot->probe_pad = gst_element_get_static_pad(appsink, "sink");
    slot->probe_id = gst_pad_add_probe(slot->probe_pad, GST_PAD_PROBE_TYPE_BUFFER, slot_frame_probe, slot, NULL);

    gint flags = slot->config->audio ? (PLAY_FLAG_VIDEO | PLAY_FLAG_AUDIO | PLAY_FLAG_SOFT_VOLUME) : PLAY_FLAG_VIDEO;
    g_object_set(playbin, "uri", uri, "flags", flags, "video-sink", appsink, NULL);
    g_free(uri);

    slot->pipeline = playbin;
    slot->bus = gst_pipeline_get_bus(GST_PIPELINE(playbin));
    return true;
}

// Logs and consumes an error message waiting on the bus, if any.
static bool slot_report_bus_error(media_slot *slot) {
    GstMessage *msg = gst_bus_pop_filtered(slot->bus, GST_MESSAGE_ERROR);
    if (!msg)
        return false;
    GError *err = NULL;
    gst_message_parse_error(msg, &err, NULL);
    cflp_warning("Slideshow: cannot play %s: %s", slot->name, err ? err->message : "unknown error");
    g_clear_error(&err);
    gst_message_unref(msg);
    return true;
}

// Waits for the pipeline to finish prerolling, checking for shutdown.
static bool slot_wait_preroll(media_slot *slot) {
    int64_t deadline = g_get_monotonic_time() + SLOT_PREROLL_TIMEOUT_US;
    while (!atomic_load(&slot->stopping) && g_get_monotonic_time() < deadline) {
        GstStateChangeReturn ret = gst_element_get_state(slot->pipeline, NULL, NULL, 100 * GST_MSECOND);
        if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL)
            return true;
        if (ret == GST_STATE_CHANGE_FAILURE) {
            if (!slot_report_bus_error(slot))
                cflp_warning("Slideshow: cannot play %s", slot->name);
            return false;
        }
        if (slot_report_bus_error(slot))
            return false;
    }
    if (!atomic_load(&slot->stopping))
        cflp_warning("Slideshow: timed out decoding %s", slot->name);
    return false;
}

static bool slot_prepare(media_slot *slot) {
    if (!slot_load_source(slot) || !slot_build_pipeline(slot))
        return false;
    if (gst_element_set_state(slot->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        slot_report_bus_error(slot);
        return false;
    }
    if (!slot_wait_preroll(slot))
        return false;

    // A flushing segment seek makes the pipeline post SEGMENT_DONE instead of
    // EOS at the end, which allows gapless loops (see slot_handle_message)
    if (!slot->still) {
        if (gst_element_seek(slot->pipeline, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_SEGMENT,
                             GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)) {
            if (!slot_wait_preroll(slot))
                return false;
        } else if (slot->config->verbose) {
            cflp_warning("Slideshow: %s is not seekable, looping on end of stream", slot->name);
        }
    }

    if (!slot_has_frame(slot)) {
        cflp_warning("Slideshow: %s produced no picture", slot->name);
        return false;
    }
    return true;
}

static void slot_seek_start(media_slot *slot, GstSeekFlags flags) {
    if (!gst_element_seek(slot->pipeline, 1.0, GST_FORMAT_TIME, flags, GST_SEEK_TYPE_SET, 0,
                          GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE) && slot->config->verbose)
        cflp_warning("Slideshow: loop seek failed for %s", slot->name);
}

// GIF demuxers mishandle non-flushing segment seeks, and post SEGMENT_DONE
// before the last frames have been displayed. Wait out the rest of the loop
// before the flushing seek so the animation keeps its speed (the same
// approach as the single-wallpaper GIF path in main.c).
static void slot_loop_gif(media_slot *slot) {
    gint64 duration = 0;
    if (gst_element_query_duration(slot->pipeline, GST_FORMAT_TIME, &duration) && duration > 0) {
        int64_t remaining_us = duration / 1000 - (g_get_monotonic_time() - slot->loop_start_us);
        if (remaining_us > 30 * G_USEC_PER_SEC)
            remaining_us = 0;
        while (remaining_us > 0 && !atomic_load(&slot->stopping)) {
            int64_t chunk = remaining_us < 50000 ? remaining_us : 50000;
            g_usleep(chunk);
            remaining_us -= chunk;
        }
    }
    slot->loop_start_us = g_get_monotonic_time();
    slot_seek_start(slot, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_SEGMENT);
}

// Returns false when the slot can no longer play.
static bool slot_handle_message(media_slot *slot, GstMessage *msg) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gst_message_parse_error(msg, &err, NULL);
            cflp_warning("Slideshow: error playing %s: %s", slot->name, err ? err->message : "unknown error");
            g_clear_error(&err);
            return false;
        }
        case GST_MESSAGE_SEGMENT_DONE:
            if (slot->gif)
                slot_loop_gif(slot);
            else
                slot_seek_start(slot, GST_SEEK_FLAG_SEGMENT);
            break;
        case GST_MESSAGE_EOS:
            // Stills end after their one frame, which stays on screen
            if (!slot->still)
                slot_seek_start(slot, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_SEGMENT);
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(slot->pipeline) && slot->loop_start_us == 0) {
                GstState new_state;
                gst_message_parse_state_changed(msg, NULL, &new_state, NULL);
                if (new_state == GST_STATE_PLAYING)
                    slot->loop_start_us = g_get_monotonic_time();
            }
            break;
        default:
            break;
    }
    return true;
}

static void *slot_thread_main(void *arg) {
    media_slot *slot = arg;
    if (!slot_prepare(slot)) {
        atomic_store(&slot->status, SLOT_FAILED);
        wake_main_loop(slot);
        return NULL;
    }
    atomic_store(&slot->status, SLOT_READY);
    wake_main_loop(slot);

    while (!atomic_load(&slot->stopping)) {
        GstMessage *msg = gst_bus_timed_pop(slot->bus, 100 * GST_MSECOND);
        if (!msg)
            continue;
        bool healthy = slot_handle_message(slot, msg);
        gst_message_unref(msg);
        if (!healthy) {
            atomic_store(&slot->status, SLOT_FAILED);
            wake_main_loop(slot);
            break;
        }
    }
    return NULL;
}

static media_slot *slot_new(slideshow *show, const char *name) {
    media_slot *slot = calloc(1, sizeof(*slot));
    if (!slot)
        return NULL;
    slot->id = ++show->next_slot_id;
    slot->name = strdup(name);
    slot->path = g_build_filename(show->config.directory, name, NULL);
    slot->config = &show->config;
    slot->memfd = -1;
    atomic_init(&slot->status, SLOT_PREPARING);
    atomic_init(&slot->stopping, false);
    pthread_mutex_init(&slot->frame_lock, NULL);

    const char *ext = strrchr(name, '.');
    slot->gif = ext && g_ascii_strcasecmp(ext, ".gif") == 0;
    // Stills are images other than GIFs, which play as animations
    slot->still = media_is_image_name(name) && !slot->gif;

    if (!slot->name || !slot->path ||
        !(slot->thread_started = pthread_create(&slot->thread, NULL, slot_thread_main, slot) == 0))
        atomic_store(&slot->status, SLOT_FAILED);
    if (show->config.verbose)
        cflp_info("Slideshow: preparing %s", name);
    return slot;
}

static void slot_destroy(media_slot *slot) {
    atomic_store(&slot->stopping, true);
    if (slot->thread_started)
        pthread_join(slot->thread, NULL);
    if (slot->pipeline)
        gst_element_set_state(slot->pipeline, GST_STATE_NULL);
    // Someone else may still hold the pipeline (main.c keeps a reference to
    // the one on screen); the probe must never outlive the slot it points to
    if (slot->probe_pad) {
        gst_pad_remove_probe(slot->probe_pad, slot->probe_id);
        gst_object_unref(slot->probe_pad);
    }
    if (slot->pipeline)
        gst_object_unref(slot->pipeline);
    if (slot->bus)
        gst_object_unref(slot->bus);
    if (slot->frame)
        gst_buffer_unref(slot->frame);
    if (slot->caps)
        gst_caps_unref(slot->caps);
    if (slot->memfd >= 0)
        close(slot->memfd);
    pthread_mutex_destroy(&slot->frame_lock);
    free(slot->name);
    g_free(slot->path);
    free(slot);
}

static void *slot_teardown_main(void *arg) {
    slot_destroy(arg);
    atomic_fetch_sub(&teardowns_in_flight, 1);
    return NULL;
}

// Tears the slot down on a separate thread: stopping a pipeline can take a
// moment, and the main thread is busy drawing the crossfade's last frames.
static void slot_release(media_slot *slot) {
    if (!slot)
        return;
    pthread_t thread;
    atomic_fetch_add(&teardowns_in_flight, 1);
    if (pthread_create(&thread, NULL, slot_teardown_main, slot) == 0) {
        pthread_detach(thread);
    } else {
        slot_teardown_main(slot);
    }
}

static slot_status slot_get_status(const media_slot *slot) {
    return (slot_status)atomic_load(&slot->status);
}

static void slot_set_playing(media_slot *slot, bool playing) {
    gst_element_set_state(slot->pipeline, playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
}

static void slot_set_volume(media_slot *slot, double volume) {
    if (slot->config->audio)
        g_object_set(slot->pipeline, "volume", volume, NULL);
}

uint64_t media_slot_id(const media_slot *slot) {
    return slot->id;
}

GstBuffer *media_slot_take_frame(media_slot *slot, int *width, int *height) {
    GstBuffer *frame = NULL;
    pthread_mutex_lock(&slot->frame_lock);
    if (slot->frame_new && slot->frame) {
        frame = gst_buffer_ref(slot->frame);
        *width = slot->frame_width;
        *height = slot->frame_height;
        slot->frame_new = false;
    }
    pthread_mutex_unlock(&slot->frame_lock);
    return frame;
}

// ==== Scheduler ====

static bool name_set_contains(const name_set *set, const char *name) {
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->names[i], name) == 0)
            return true;
    }
    return false;
}

static void name_set_add(name_set *set, const char *name) {
    if (name_set_contains(set, name))
        return;
    char **names = realloc(set->names, (set->count + 1) * sizeof(*names));
    if (!names)
        return;
    set->names = names;
    if ((set->names[set->count] = strdup(name)) != NULL)
        set->count++;
}

static void name_set_clear(name_set *set) {
    for (size_t i = 0; i < set->count; i++)
        free(set->names[i]);
    free(set->names);
    *set = (name_set){0};
}

static void forget_failures(slideshow *show) {
    name_set_clear(&show->failed);
}

static bool skip_failed(const char *name, void *userdata) {
    const slideshow *show = userdata;
    return name_set_contains(&show->failed, name);
}

// Shuffle draws only files not yet shown this round, and never the current
// one, so nothing repeats back to back when a new round starts
static bool skip_for_shuffle(const char *name, void *userdata) {
    const slideshow *show = userdata;
    return skip_failed(name, userdata) || name_set_contains(&show->played, name) ||
           (show->current && strcmp(show->current->name, name) == 0);
}

static unsigned random_below(unsigned bound, void *userdata) {
    (void)userdata;
    return (unsigned)g_random_int_range(0, (gint32)bound);
}

// The name of the item to show after the current one.
static char *pick_next_name(slideshow *show) {
    const char *directory = show->config.directory;
    time_t now = time(NULL);
    if (show->config.shuffle) {
        char *name = playlist_random(directory, now, skip_for_shuffle, show, random_below, NULL);
        if (!name && show->played.count > 0) {
            // Everything has been shown this round: start the next one
            name_set_clear(&show->played);
            name = playlist_random(directory, now, skip_for_shuffle, show, random_below, NULL);
        }
        if (name)
            return name;
        // Only the current item is left; the sequential pick below returns
        // it so that it keeps looping
    }
    return playlist_next(directory, show->current ? show->current->name : NULL, now, skip_failed, show);
}

static double fade_progress(const slideshow *show, double elapsed) {
    if (show->config.fade_secs <= 0)
        return 1.0;
    double progress = elapsed / show->config.fade_secs;
    return progress < 0 ? 0 : progress > 1 ? 1 : progress;
}

// Makes the incoming slot the current one and drops the outgoing slot.
static void finish_switch(slideshow *show) {
    slot_release(show->current);
    show->current = show->next;
    show->next = NULL;
    show->current_shown = show->fading ? show->fade_elapsed : 0;
    show->fading = false;
    slot_set_volume(show->current, 1.0);
    forget_failures(show);
    if (show->config.shuffle)
        name_set_add(&show->played, show->current->name);
    if (show->config.verbose)
        cflp_info("Slideshow: showing %s", show->current->name);
}

static void begin_switch(slideshow *show) {
    bool fade = show->current && show->config.fade_secs > 0;
    slot_set_volume(show->next, fade ? 0.0 : 1.0);
    slot_set_playing(show->next, !show->paused);
    show->fading = fade;
    show->fade_elapsed = 0;
    if (!fade)
        finish_switch(show);
}

// Picks the item after the current one and starts loading it.
static void choose_next(slideshow *show, int64_t now_us, bool current_usable) {
    char *name = pick_next_name(show);
    if (!name) {
        if (!show->warned_empty)
            cflp_warning("Slideshow: no playable images or videos in %s, checking again shortly",
                         show->config.directory);
        show->warned_empty = true;
        show->retry_at_us = now_us + (int64_t)(SLIDESHOW_RETRY_SECS * G_USEC_PER_SEC);
        // Give files that failed another chance on the next scan
        forget_failures(show);
        return;
    }
    show->warned_empty = false;

    if (current_usable && strcmp(name, show->current->name) == 0) {
        // The current item is the only one: keep looping it, look again in
        // one interval, and give failed files another chance then
        show->current_shown = 0;
        forget_failures(show);
    } else {
        show->next = slot_new(show, name);
    }
    free(name);
}

slideshow *slideshow_new(slideshow_config config) {
    slideshow *show = calloc(1, sizeof(*show));
    char *directory = strdup(config.directory);
    if (!show || !directory) {
        free(show);
        free(directory);
        return NULL;
    }
    show->config = config;
    show->config.directory = directory;
    return show;
}

void slideshow_free(slideshow *show) {
    if (!show)
        return;
    if (show->current)
        slot_destroy(show->current);
    if (show->next)
        slot_destroy(show->next);
    // Slots being torn down asynchronously still read show->config, so
    // let them finish before freeing it
    for (int waited_ms = 0; atomic_load(&teardowns_in_flight) > 0 && waited_ms < 5000; waited_ms += 10)
        g_usleep(10000);
    forget_failures(show);
    name_set_clear(&show->played);
    free((char *)show->config.directory);
    free(show);
}

bool slideshow_tick(slideshow *show, int64_t now_us, bool paused) {
    double dt = show->last_tick_us ? (double)(now_us - show->last_tick_us) / G_USEC_PER_SEC : 0;
    show->last_tick_us = now_us;
    if (!show->paused) {
        show->current_shown += dt;
        if (show->fading)
            show->fade_elapsed += dt;
    }
    bool changed = false;

    if (paused != show->paused) {
        show->paused = paused;
        if (show->current)
            slot_set_playing(show->current, !paused);
        if (show->fading)
            slot_set_playing(show->next, !paused);
    }

    // An incoming item that failed is dropped (mid-fade the outgoing one
    // simply stays), and another one is tried
    if (show->next && slot_get_status(show->next) == SLOT_FAILED) {
        name_set_add(&show->failed, show->next->name);
        slot_release(show->next);
        show->next = NULL;
        changed |= show->fading;
        show->fading = false;
    }

    // A current item that fails keeps its last frame on screen and gives
    // way to the next one as soon as it is ready
    bool current_usable = show->current && slot_get_status(show->current) != SLOT_FAILED;
    double interval = show->config.interval_secs;
    bool turn_over = !current_usable || show->current_shown >= interval;

    if (!show->next && now_us >= show->retry_at_us &&
        (turn_over || show->current_shown >= interval - SLIDESHOW_PRELOAD_LEAD_SECS))
        choose_next(show, now_us, current_usable);

    if (show->next && !show->fading && turn_over && slot_get_status(show->next) == SLOT_READY) {
        begin_switch(show);
        changed = true;
    }

    if (show->fading) {
        double progress = fade_progress(show, show->fade_elapsed);
        slot_set_volume(show->current, 1.0 - progress);
        slot_set_volume(show->next, progress);
        if (progress >= 1.0) {
            finish_switch(show);
            changed = true;
        }
    }
    return changed;
}

size_t slideshow_layers(const slideshow *show, int64_t now_us, slideshow_layer out[2]) {
    size_t count = 0;
    if (!show->fading) {
        if (show->current)
            out[count++] = (slideshow_layer){show->current, 1.0f};
        return count;
    }
    // Interpolate between ticks so the fade is as smooth as the frame rate
    double since_tick = show->paused ? 0 : (double)(now_us - show->last_tick_us) / G_USEC_PER_SEC;
    float progress = (float)fade_progress(show, show->fade_elapsed + since_tick);
    if (show->current)
        out[count++] = (slideshow_layer){show->current, 1.0f - progress};
    out[count++] = (slideshow_layer){show->next, progress};
    return count;
}

bool slideshow_fading(const slideshow *show) {
    return show->fading;
}

GstElement *slideshow_current_pipeline(const slideshow *show) {
    return show->current ? show->current->pipeline : NULL;
}

const char *slideshow_current_name(const slideshow *show) {
    return show->current ? show->current->name : NULL;
}
