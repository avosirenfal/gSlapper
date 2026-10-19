#ifndef SLIDESHOW_H
#define SLIDESHOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gst/gst.h>

// Folder slideshow: shows every image and video in a folder in turn, each
// for a fixed time, crossfading from one to the next.
//
// Each item plays in its own "slot", a GStreamer pipeline. Well before an
// item's turn, its slot copies the file into memory and prerolls it, so the
// first frame is decoded and waiting when the crossfade starts. A slot does
// its loading and bus handling on its own thread; the scheduler and the
// accessors below belong to the main thread.

typedef struct {
    const char *directory;      // copied by slideshow_new
    double interval_secs;       // time from one switch to the next
    double fade_secs;           // crossfade length; 0 cuts directly
    size_t preload_limit_bytes; // larger files stream from disk instead of RAM
    bool audio;
    bool shuffle;               // shuffled-deck order: each file once per round
    int wakeup_fd;              // written to when a slot has a new frame
    int verbose;
} slideshow_config;

typedef struct slideshow slideshow;
typedef struct media_slot media_slot;

// A slot to draw, bottom to top, with its opacity.
typedef struct {
    media_slot *slot;
    float opacity;
} slideshow_layer;

slideshow *slideshow_new(slideshow_config config);
// Stops every slot and waits for their threads.
void slideshow_free(slideshow *show);

// How the slideshow should run right now.
typedef enum {
    SLIDESHOW_PLAYING,
    // Playback paused for an automatic reason (a fullscreen window, the
    // wallpaper hidden, no monitor): items keep switching on schedule, so
    // the rotation is where it should be once the wallpaper shows again
    SLIDESHOW_PAUSED,
    // Paused on request (IPC pause): the clock stops too, and the current
    // item stays until playback resumes
    SLIDESHOW_FROZEN,
} slideshow_playback;

// Advances the schedule to now_us (g_get_monotonic_time()). Returns true
// when the set of visible layers changed and every output should be redrawn.
bool slideshow_tick(slideshow *show, int64_t now_us, slideshow_playback playback);

// Fills up to two layers for drawing at now_us; returns how many.
size_t slideshow_layers(const slideshow *show, int64_t now_us, slideshow_layer out[2]);
bool slideshow_fading(const slideshow *show);

// The pipeline of the item currently on screen, or NULL before the first
// item is ready. Borrowed; take a reference to keep it.
GstElement *slideshow_current_pipeline(const slideshow *show);
// File name of the item currently on screen, or NULL.
const char *slideshow_current_name(const slideshow *show);

// Stable identity for associating render resources with a slot.
uint64_t media_slot_id(const media_slot *slot);
// Returns the newest frame since the last call, or NULL. The caller owns
// the returned reference. Frames are RGBA, width * 4 bytes per row.
GstBuffer *media_slot_take_frame(media_slot *slot, int *width, int *height);

#endif // SLIDESHOW_H
