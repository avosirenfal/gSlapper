// Exercises the folder playlist and the slideshow scheduler with real
// GStreamer pipelines on generated media. Exits 77 (skipped) when the
// encoders needed to generate test media are not installed.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gst/gst.h>

#include "playlist.h"
#include "slideshow.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static char dir[] = "/tmp/gslapper-slideshow-XXXXXX";

static char *path_in(const char *name) {
    return g_build_filename(dir, name, NULL);
}

// Back-dates a file past the playlist's settle time.
static void settle(const char *name) {
    char *path = path_in(name);
    struct timespec times[2] = {{.tv_sec = time(NULL) - 60}, {.tv_sec = time(NULL) - 60}};
    utimensat(AT_FDCWD, path, times, 0);
    g_free(path);
}

static void write_file(const char *name, const char *contents) {
    char *path = path_in(name);
    g_file_set_contents(path, contents, -1, NULL);
    g_free(path);
}

static bool generate(const char *name, const char *description) {
    char *path = path_in(name);
    char *launch = g_strdup_printf("%s ! filesink location=\"%s\"", description, path);
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(launch, &err);
    bool ok = pipeline && !err;
    if (ok) {
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *msg = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
        if (msg)
            gst_message_unref(msg);
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    if (pipeline)
        gst_object_unref(pipeline);
    g_clear_error(&err);
    g_free(launch);
    g_free(path);
    if (ok)
        settle(name);
    return ok;
}

static bool generate_video(const char *name, const char *pattern) {
    char *description = g_strdup_printf(
        "videotestsrc num-buffers=20 pattern=%s ! video/x-raw,width=64,height=48,framerate=20/1 ! "
        "vp8enc ! webmmux", pattern);
    bool ok = generate(name, description);
    g_free(description);
    return ok;
}

static bool generate_image(const char *name, const char *pattern) {
    char *description = g_strdup_printf(
        "videotestsrc num-buffers=1 pattern=%s ! video/x-raw,width=40,height=30 ! pngenc", pattern);
    bool ok = generate(name, description);
    g_free(description);
    return ok;
}

// ---- Playlist ----

static bool skip_b(const char *name, void *userdata) {
    (void)userdata;
    return strcmp(name, "b.png") == 0;
}

static void test_playlist(void) {
    write_file("clip2.mp4", "x");
    write_file("clip10.mp4", "x");
    write_file(".hidden.mp4", "x");
    write_file("notes.txt", "x");
    write_file("fresh.mp4", "x"); // just written, so still settling
    settle("clip2.mp4");
    settle("clip10.mp4");
    settle(".hidden.mp4");
    settle("notes.txt");
    char *sub = path_in("sub.mp4");
    mkdir(sub, 0755);
    g_free(sub);

    char *first = playlist_next(dir, NULL, time(NULL), NULL, NULL);
    char *after_first = playlist_next(dir, first, time(NULL), NULL, NULL);
    // Natural order: a.webm, b.png, clip2, clip10, d.webm, then wraps
    CHECK(first && strcmp(first, "a.webm") == 0);
    CHECK(after_first && strcmp(after_first, "b.png") == 0);
    char *skipped = playlist_next(dir, "a.webm", time(NULL), skip_b, NULL);
    CHECK(skipped && strcmp(skipped, "clip2.mp4") == 0);
    char *natural = playlist_next(dir, "clip2.mp4", time(NULL), NULL, NULL);
    CHECK(natural && strcmp(natural, "clip10.mp4") == 0);
    char *wrapped = playlist_next(dir, "d.webm", time(NULL), NULL, NULL);
    CHECK(wrapped && strcmp(wrapped, "a.webm") == 0);
    // A deleted current item still has a well-defined successor
    char *after_gone = playlist_next(dir, "c_gone.mp4", time(NULL), NULL, NULL);
    CHECK(after_gone && strcmp(after_gone, "clip2.mp4") == 0);
    CHECK(playlist_next("/nonexistent/folder", NULL, time(NULL), NULL, NULL) == NULL);
    // Once settled, the fresh file is a candidate
    char *later = playlist_next(dir, "d.webm", time(NULL) + 2 * PLAYLIST_SETTLE_SECS, NULL, NULL);
    CHECK(later && strcmp(later, "fresh.mp4") == 0);

    free(first);
    free(after_first);
    free(skipped);
    free(natural);
    free(wrapped);
    free(after_gone);
    free(later);

    const char *cleanup[] = {"clip2.mp4", "clip10.mp4", ".hidden.mp4", "notes.txt", "fresh.mp4"};
    for (size_t i = 0; i < sizeof(cleanup) / sizeof(cleanup[0]); i++) {
        char *path = path_in(cleanup[i]);
        unlink(path);
        g_free(path);
    }
    sub = path_in("sub.mp4");
    rmdir(sub);
    g_free(sub);

    CHECK(media_is_video_name("A.MKV") && media_is_image_name("x.JPeG") && !media_is_image_name("gif"));
}

// ---- Scheduler ----

typedef struct {
    char shown[16][64];
    size_t shown_count;
    int frames;
    bool saw_two_layers;
    float max_incoming_opacity_seen;
} run_log;

static void drain(int fd) {
    char buffer[256];
    while (read(fd, buffer, sizeof(buffer)) > 0) {
    }
}

// Ticks the slideshow like the main loop does, for `secs`, consuming frames
// as the renderer would and recording each item that comes on screen.
static void run_for(slideshow *show, int wakeup_fd, double secs, bool paused, run_log *log) {
    int64_t end = g_get_monotonic_time() + (int64_t)(secs * G_USEC_PER_SEC);
    while (g_get_monotonic_time() < end) {
        int64_t now = g_get_monotonic_time();
        slideshow_tick(show, now, paused);
        const char *name = slideshow_current_name(show);
        if (name && (log->shown_count == 0 || strcmp(log->shown[log->shown_count - 1], name) != 0) &&
            log->shown_count < 16)
            g_strlcpy(log->shown[log->shown_count++], name, sizeof(log->shown[0]));

        slideshow_layer layers[2];
        size_t count = slideshow_layers(show, now, layers);
        if (count == 2) {
            log->saw_two_layers = true;
            if (layers[1].opacity > log->max_incoming_opacity_seen && layers[1].opacity < 1.0f)
                log->max_incoming_opacity_seen = layers[1].opacity;
        }
        for (size_t i = 0; i < count; i++) {
            int w, h;
            GstBuffer *frame = media_slot_take_frame(layers[i].slot, &w, &h);
            if (frame) {
                CHECK(w > 0 && h > 0 && gst_buffer_get_size(frame) >= (gsize)w * h * 4);
                log->frames++;
                gst_buffer_unref(frame);
            }
        }
        drain(wakeup_fd);
        g_usleep(10000);
    }
}

static void test_rotation(void) {
    int wakeup[2];
    CHECK(pipe2(wakeup, O_NONBLOCK | O_CLOEXEC) == 0);
    write_file("c_broken.mp4", "this is not a video");
    settle("c_broken.mp4");

    slideshow *show = slideshow_new((slideshow_config){
        .directory = dir,
        .interval_secs = 1.0,
        .fade_secs = 0.3,
        .preload_limit_bytes = 64 * 1024 * 1024,
        .audio = false,
        .wakeup_fd = wakeup[1],
        .verbose = 1,
    });

    // a.webm, b.png, c_broken.mp4 (skipped), d.webm, then a.webm again
    run_log log = {0};
    run_for(show, wakeup[0], 4.6, false, &log);
    CHECK(log.shown_count >= 4);
    const char *expected[] = {"a.webm", "b.png", "d.webm", "a.webm"};
    for (size_t i = 0; i < 4 && i < log.shown_count; i++) {
        if (strcmp(log.shown[i], expected[i]) != 0) {
            fprintf(stderr, "shown[%zu] = %s, expected %s\n", i, log.shown[i], expected[i]);
            failures++;
        }
    }
    CHECK(log.saw_two_layers);
    CHECK(log.max_incoming_opacity_seen > 0.2f);
    CHECK(log.frames > 10);

    // Pausing freezes the rotation
    const char *before_pause = slideshow_current_name(show);
    char *paused_name = before_pause ? strdup(before_pause) : NULL;
    run_log paused_log = {0};
    run_for(show, wakeup[0], 2.0, true, &paused_log);
    CHECK(paused_name && slideshow_current_name(show) && strcmp(paused_name, slideshow_current_name(show)) == 0);
    free(paused_name);

    // Change the folder underneath it: delete every original, add one image
    const char *originals[] = {"a.webm", "b.png", "c_broken.mp4", "d.webm"};
    for (size_t i = 0; i < 4; i++) {
        char *path = path_in(originals[i]);
        unlink(path);
        g_free(path);
    }
    CHECK(generate_image("e.png", "ball"));
    run_log changed = {0};
    run_for(show, wakeup[0], 3.0, false, &changed);
    // The item already playing came from memory, so it survived its file's
    // deletion; afterwards the rotation settles on the one remaining file
    CHECK(slideshow_current_name(show) && strcmp(slideshow_current_name(show), "e.png") == 0);

    // An empty folder keeps the last item on screen
    char *e = path_in("e.png");
    unlink(e);
    g_free(e);
    run_log emptied = {0};
    run_for(show, wakeup[0], 2.5, false, &emptied);
    CHECK(slideshow_current_name(show) && strcmp(slideshow_current_name(show), "e.png") == 0);

    slideshow_free(show);
    close(wakeup[0]);
    close(wakeup[1]);
}

int main(void) {
    gst_init(NULL, NULL);
    if (!mkdtemp(dir))
        return 1;

    if (!generate_video("a.webm", "smpte") || !generate_image("b.png", "snow") ||
        !generate_video("d.webm", "ball")) {
        fprintf(stderr, "cannot generate test media (needs vp8enc, webmmux, pngenc); skipping\n");
        return 77;
    }

    test_playlist();
    test_rotation();

    char *cmd = g_strdup_printf("rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", dir);
    g_free(cmd);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("slideshow tests passed\n");
    return EXIT_SUCCESS;
}
