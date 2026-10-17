#ifndef NIRI_H
#define NIRI_H

#include <stdbool.h>

#include "json.h"

// Tracks niri's window layout over its IPC socket and reports whether a
// fullscreen window is focused, so playback can pause while the wallpaper
// is covered.
//
// niri's event stream starts with a full snapshot of workspaces and windows
// and then sends incremental updates. Outputs are not part of the stream, so
// they are queried separately whenever they may have changed. See
// https://yalter.github.io/niri/niri_ipc/ for the protocol.

// ---- State tracking (pure, no IO) ----

typedef struct niri_state niri_state;

typedef enum {
    // The event was applied, or is not one this tracker needs.
    NIRI_EVENT_OK,
    // A known event had an unexpected shape. The tracked state may no longer
    // match niri's; the caller should resubscribe to get a fresh snapshot.
    NIRI_EVENT_MALFORMED,
} niri_event_result;

niri_state *niri_state_new(void);
void niri_state_free(niri_state *state);

// Applies one event-stream message, e.g. {"WindowClosed":{"id":3}}.
// Unknown events and unknown fields are ignored.
niri_event_result niri_state_apply_event(niri_state *state, const json_value *event);

// True when the output list must be re-queried before should_pause can be
// trusted: initially, after workspaces change (outputs were connected or
// disconnected), and when the focused window's size or workspace changes
// (its output may have been reconfigured).
bool niri_state_outputs_stale(const niri_state *state);

// Replaces the outputs from the payload of an Outputs reply, a map from
// output name to output. Returns false, leaving the state untouched, when
// the payload is not such a map.
bool niri_state_set_outputs(niri_state *state, const json_value *outputs);

// True when the focused window's tile covers its whole output and the
// overview (which shows the wallpaper) is closed.
bool niri_state_should_pause(const niri_state *state);

// ---- Monitor thread ----

typedef void (*niri_pause_callback)(bool should_pause, void *userdata);

typedef struct {
    const char *socket_path; // usually $NIRI_SOCKET; must outlive the thread
    niri_pause_callback on_change; // called from the monitor thread
    void *userdata;
    int verbose;
} niri_monitor_config;

// pthread entry point taking a niri_monitor_config. Calls on_change whenever
// the pause decision changes. Never returns: when the connection is lost it
// reports should_pause = false and reconnects with backoff. Cancellation is
// only honored while it waits on the socket or between reconnects.
void *niri_monitor_thread(void *config);

#endif // NIRI_H
