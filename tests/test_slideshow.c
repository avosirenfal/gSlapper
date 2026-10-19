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

#define LOG_CAPACITY 32

typedef struct {
    char shown[LOG_CAPACITY][64]; // each item as it comes on screen
    size_t shown_count;
    uint64_t last_slot_id;
    int frames;
    bool saw_two_layers;
    float max_incoming_opacity_seen;
} run_log;

static void drain(int fd) {
    char buffer[256];
    while (read(fd, buffer, sizeof(buffer)) > 0) {
    }
}

// Ticks the slideshow like the main loop does, for `secs` or until
// `stop_at` items have been shown (0 for no limit), consuming frames as the
// renderer would and recording each item that comes on screen. Items are
// told apart by slot, so the same file shown twice in a row counts twice.
static void run_for(slideshow *show, int wakeup_fd, double secs, slideshow_playback playback, size_t stop_at,
                    run_log *log) {
    int64_t end = g_get_monotonic_time() + (int64_t)(secs * G_USEC_PER_SEC);
    while (g_get_monotonic_time() < end && (stop_at == 0 || log->shown_count < stop_at)) {
        int64_t now = g_get_monotonic_time();
        slideshow_tick(show, now, playback);

        slideshow_layer layers[2];
        size_t count = slideshow_layers(show, now, layers);
        const char *name = slideshow_current_name(show);
        if (count == 1 && name && media_slot_id(layers[0].slot) != log->last_slot_id &&
            log->shown_count < LOG_CAPACITY) {
            log->last_slot_id = media_slot_id(layers[0].slot);
            g_strlcpy(log->shown[log->shown_count++], name, sizeof(log->shown[0]));
        }
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

static unsigned glib_random_below(unsigned bound, void *userdata) {
    (void)userdata;
    return (unsigned)g_random_int_range(0, (gint32)bound);
}

static void test_playlist_random(void) {
    // a.webm, b.png and d.webm exist; b.png is skipped
    int counts[3] = {0};
    const char *names[3] = {"a.webm", "b.png", "d.webm"};
    for (int i = 0; i < 300; i++) {
        char *name = playlist_random(dir, time(NULL), skip_b, NULL, glib_random_below, NULL);
        for (int j = 0; j < 3; j++)
            counts[j] += name && strcmp(name, names[j]) == 0;
        free(name);
    }
    // Each of the two candidates is drawn about 150 times
    CHECK(counts[0] > 100 && counts[2] > 100 && counts[1] == 0);
    CHECK(playlist_random("/nonexistent/folder", time(NULL), NULL, NULL, glib_random_below, NULL) == NULL);
}

static void test_shuffle(void) {
    int wakeup[2];
    CHECK(pipe2(wakeup, O_NONBLOCK | O_CLOEXEC) == 0);
    write_file("c_broken.mp4", "this is not a video");
    settle("c_broken.mp4");

    slideshow *show = slideshow_new((slideshow_config){
        .directory = dir,
        .interval_secs = 0.5,
        .fade_secs = 0.1,
        .preload_limit_bytes = 64 * 1024 * 1024,
        .shuffle = true,
        .wakeup_fd = wakeup[1],
    });
    run_log log = {0};
    run_for(show, wakeup[0], 6.0, SLIDESHOW_PLAYING, 0, &log);
    slideshow_free(show);

    // Rounds of the three playable files, each a permutation, never the
    // broken file, and no item twice in a row across round boundaries
    CHECK(log.shown_count >= 6);
    for (size_t round = 0; round + 3 <= log.shown_count && round < 6; round += 3) {
        bool seen[3] = {false};
        const char *names[3] = {"a.webm", "b.png", "d.webm"};
        for (size_t i = round; i < round + 3; i++) {
            for (int j = 0; j < 3; j++)
                seen[j] |= strcmp(log.shown[i], names[j]) == 0;
        }
        if (!(seen[0] && seen[1] && seen[2])) {
            fprintf(stderr, "round at %zu is not a permutation: %s %s %s\n", round, log.shown[round],
                    log.shown[round + 1], log.shown[round + 2]);
            failures++;
        }
    }
    for (size_t i = 0; i < log.shown_count; i++)
        CHECK(strcmp(log.shown[i], "c_broken.mp4") != 0);

    char *broken = path_in("c_broken.mp4");
    unlink(broken);
    g_free(broken);
    close(wakeup[0]);
    close(wakeup[1]);
}

// True when names[from..from+count) holds each of `expected` exactly once.
static bool is_permutation(const run_log *log, size_t from, size_t count, const char *const *expected) {
    if (from + count > log->shown_count)
        return false;
    for (size_t e = 0; e < count; e++) {
        size_t hits = 0;
        for (size_t i = from; i < from + count; i++)
            hits += strcmp(log->shown[i], expected[e]) == 0;
        if (hits != 1)
            return false;
    }
    return true;
}

static void print_log(const char *label, const run_log *log) {
    fprintf(stderr, "%s:", label);
    for (size_t i = 0; i < log->shown_count; i++)
        fprintf(stderr, " %s", log->shown[i]);
    fprintf(stderr, "\n");
}

// Shuffle behaves like a shuffled deck: no file repeats until every file has
// been shown, and a file added mid-round joins the rest of that round.
static void test_shuffle_deck(void) {
    char *deck_dir = g_build_filename(dir, "deck", NULL);
    mkdir(deck_dir, 0755);
    const char *names[] = {"s1.png", "s2.png", "s3.png", "s4.png", "s5.png", "s6.png", "s7_added_later.png"};
    for (int i = 0; i < 6; i++) {
        char *name = g_build_filename("deck", names[i], NULL);
        CHECK(generate_image(name, "smpte"));
        g_free(name);
    }

    int wakeup[2];
    CHECK(pipe2(wakeup, O_NONBLOCK | O_CLOEXEC) == 0);
    slideshow *show = slideshow_new((slideshow_config){
        .directory = deck_dir,
        .interval_secs = 0.3,
        .fade_secs = 0,
        .preload_limit_bytes = 64 * 1024 * 1024,
        .shuffle = true,
        .wakeup_fd = wakeup[1],
    });

    // Three items into the first round, a seventh file appears
    run_log log = {0};
    run_for(show, wakeup[0], 10.0, SLIDESHOW_PLAYING, 3, &log);
    char *added = g_build_filename("deck", names[6], NULL);
    CHECK(generate_image(added, "ball"));
    g_free(added);
    run_for(show, wakeup[0], 20.0, SLIDESHOW_PLAYING, 7 + 7 + 1, &log);
    slideshow_free(show);

    // Round one: all seven files once, the late one included; round two:
    // all seven once again
    if (!is_permutation(&log, 0, 7, names) || !is_permutation(&log, 7, 7, names)) {
        print_log("shuffle deck", &log);
        failures++;
    }
    for (size_t i = 1; i < log.shown_count; i++)
        CHECK(strcmp(log.shown[i], log.shown[i - 1]) != 0);

    close(wakeup[0]);
    close(wakeup[1]);
    char *cmd = g_strdup_printf("rm -rf '%s'", deck_dir);
    CHECK(system(cmd) == 0);
    g_free(cmd);
    g_free(deck_dir);
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
    run_for(show, wakeup[0], 4.6, SLIDESHOW_PLAYING, 0, &log);
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

    // A requested pause freezes the rotation
    const char *before_pause = slideshow_current_name(show);
    char *paused_name = before_pause ? strdup(before_pause) : NULL;
    run_log frozen_log = {0};
    run_for(show, wakeup[0], 2.0, SLIDESHOW_FROZEN, 0, &frozen_log);
    CHECK(paused_name && slideshow_current_name(show) && strcmp(paused_name, slideshow_current_name(show)) == 0);
    CHECK(frozen_log.shown_count <= 1);
    free(paused_name);

    // An automatic pause (a fullscreen window) keeps it on schedule: two
    // seconds at one item per second brings two new items
    run_log paused_log = {0};
    run_for(show, wakeup[0], 2.3, SLIDESHOW_PAUSED, 0, &paused_log);
    if (paused_log.shown_count < 3)
        fprintf(stderr, "only %zu items while paused\n", paused_log.shown_count);
    CHECK(paused_log.shown_count >= 3); // the item on screen when it started, then two more

    // Change the folder underneath it: delete every original, add one image
    const char *originals[] = {"a.webm", "b.png", "c_broken.mp4", "d.webm"};
    for (size_t i = 0; i < 4; i++) {
        char *path = path_in(originals[i]);
        unlink(path);
        g_free(path);
    }
    CHECK(generate_image("e.png", "ball"));
    run_log changed = {0};
    run_for(show, wakeup[0], 3.0, SLIDESHOW_PLAYING, 0, &changed);
    // The item already playing came from memory, so it survived its file's
    // deletion; afterwards the rotation settles on the one remaining file
    CHECK(slideshow_current_name(show) && strcmp(slideshow_current_name(show), "e.png") == 0);

    // An empty folder keeps the last item on screen
    char *e = path_in("e.png");
    unlink(e);
    g_free(e);
    run_log emptied = {0};
    run_for(show, wakeup[0], 2.5, SLIDESHOW_PLAYING, 0, &emptied);
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
    test_playlist_random();
    test_shuffle();
    test_shuffle_deck();
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
