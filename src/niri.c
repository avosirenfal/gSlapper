#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "cflogprinter.h"
#include "niri.h"

// ==== State tracking ====

typedef struct {
    bool present;
    uint64_t value;
} opt_id;

typedef struct {
    bool present;
    double width, height;
} opt_size;

typedef struct {
    uint64_t id;
    opt_id workspace_id;
    opt_size tile; // tile size in logical pixels, including borders
} niri_window;

typedef struct {
    uint64_t id;
    char *output; // NULL while no output is connected
} niri_workspace;

typedef struct {
    char *name;
    opt_size logical; // absent while the output is disabled
} niri_output;

struct niri_state {
    niri_window *windows;
    size_t window_count;
    niri_workspace *workspaces;
    size_t workspace_count;
    niri_output *outputs;
    size_t output_count;
    // Kept apart from the windows so it can name a window whose
    // WindowOpenedOrChanged has not arrived yet; niri does not order those.
    opt_id focused_window;
    bool overview_open;
    bool outputs_stale;
};

// The inputs of should_pause that outputs are measured against. When they
// change, the output may have been reconfigured too (a scale change resizes
// fullscreen windows), so the outputs are re-queried.
typedef struct {
    bool present;
    opt_id workspace_id;
    opt_size tile;
} focus_geometry;

// A fullscreen tile matches its output exactly; allow for rounding.
#define NIRI_COVER_TOLERANCE 1.0

static opt_id parse_opt_id(const json_value *value) {
    opt_id id = {0};
    id.present = json_as_u64(value, &id.value);
    return id;
}

static opt_size parse_size_pair(const json_value *pair) {
    opt_size size = {0};
    size.present = json_as_double(json_at(pair, 0), &size.width) &&
                   json_as_double(json_at(pair, 1), &size.height);
    return size;
}

static opt_size parse_tile_size(const json_value *layout) {
    return parse_size_pair(json_get(layout, "tile_size"));
}

static char *dup_opt_string(const char *s) {
    return s ? strdup(s) : NULL;
}

// Element parsers share one signature so parse_list can build any of the
// state's arrays. key is the member name for objects and NULL for arrays.
typedef bool (*element_parser)(const json_value *value, const char *key, void *out);
typedef void (*element_destructor)(void *element);

static bool parse_window(const json_value *value, const char *key, void *out) {
    (void)key;
    niri_window *window = out;
    if (!json_as_u64(json_get(value, "id"), &window->id))
        return false;
    window->workspace_id = parse_opt_id(json_get(value, "workspace_id"));
    window->tile = parse_tile_size(json_get(value, "layout"));
    return true;
}

static bool parse_workspace(const json_value *value, const char *key, void *out) {
    (void)key;
    niri_workspace *workspace = out;
    if (!json_as_u64(json_get(value, "id"), &workspace->id))
        return false;
    const char *output = json_as_string(json_get(value, "output"));
    workspace->output = dup_opt_string(output);
    return !output || workspace->output;
}

static void free_workspace(void *element) {
    free(((niri_workspace *)element)->output);
}

static bool parse_output(const json_value *value, const char *key, void *out) {
    niri_output *output = out;
    if (!json_has_kind(value, JSON_OBJECT))
        return false;
    const char *name = json_as_string(json_get(value, "name"));
    output->name = dup_opt_string(name ? name : key);
    if (!output->name)
        return false;
    const json_value *logical = json_get(value, "logical");
    output->logical.present = json_as_double(json_get(logical, "width"), &output->logical.width) &&
                              json_as_double(json_get(logical, "height"), &output->logical.height);
    return true;
}

static void free_output(void *element) {
    free(((niri_output *)element)->name);
}

static void free_elements(void *items, size_t count, size_t size, element_destructor destroy) {
    if (destroy) {
        for (size_t i = 0; i < count; i++)
            destroy((char *)items + i * size);
    }
    free(items);
}

// Builds a new array from every element of a JSON array or object. All or
// nothing: on failure nothing is allocated and the outputs are untouched.
static bool parse_list(const json_value *container, size_t size, element_parser parse,
                       element_destructor destroy, void **items_out, size_t *count_out) {
    if (!json_has_kind(container, JSON_ARRAY) && !json_has_kind(container, JSON_OBJECT))
        return false;
    size_t count = json_count(container);
    char *items = calloc(count ? count : 1, size);
    if (!items)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (!parse(json_at(container, i), json_key_at(container, i), items + i * size)) {
            free_elements(items, i, size, destroy);
            return false;
        }
    }
    *items_out = items;
    *count_out = count;
    return true;
}

static niri_window *find_window(const niri_state *state, uint64_t id) {
    for (size_t i = 0; i < state->window_count; i++) {
        if (state->windows[i].id == id)
            return &state->windows[i];
    }
    return NULL;
}

static const niri_workspace *find_workspace(const niri_state *state, uint64_t id) {
    for (size_t i = 0; i < state->workspace_count; i++) {
        if (state->workspaces[i].id == id)
            return &state->workspaces[i];
    }
    return NULL;
}

static const niri_output *find_output(const niri_state *state, const char *name) {
    for (size_t i = 0; i < state->output_count; i++) {
        if (strcmp(state->outputs[i].name, name) == 0)
            return &state->outputs[i];
    }
    return NULL;
}

static const niri_window *focused_window(const niri_state *state) {
    return state->focused_window.present ? find_window(state, state->focused_window.value) : NULL;
}

static focus_geometry focused_geometry(const niri_state *state) {
    const niri_window *window = focused_window(state);
    if (!window)
        return (focus_geometry){0};
    return (focus_geometry){.present = true, .workspace_id = window->workspace_id, .tile = window->tile};
}

static bool opt_id_equal(opt_id a, opt_id b) {
    return a.present == b.present && (!a.present || a.value == b.value);
}

static bool opt_size_equal(opt_size a, opt_size b) {
    return a.present == b.present && (!a.present || (a.width == b.width && a.height == b.height));
}

static bool geometry_equal(focus_geometry a, focus_geometry b) {
    return a.present == b.present &&
           (!a.present || (opt_id_equal(a.workspace_id, b.workspace_id) && opt_size_equal(a.tile, b.tile)));
}

static bool json_is_focused(const json_value *window) {
    bool focused = false;
    return json_as_bool(json_get(window, "is_focused"), &focused) && focused;
}

// ---- Event handlers ----
// Each returns false when a field the event cannot be applied without is
// missing or mistyped, and then leaves the state unchanged.

static bool on_workspaces_changed(niri_state *state, const json_value *body) {
    void *items;
    size_t count;
    if (!parse_list(json_get(body, "workspaces"), sizeof(niri_workspace), parse_workspace, free_workspace,
                    &items, &count))
        return false;
    free_elements(state->workspaces, state->workspace_count, sizeof(niri_workspace), free_workspace);
    state->workspaces = items;
    state->workspace_count = count;
    // Workspaces move between outputs when outputs come and go
    state->outputs_stale = true;
    return true;
}

static bool on_windows_changed(niri_state *state, const json_value *body) {
    const json_value *list = json_get(body, "windows");
    void *items;
    size_t count;
    if (!parse_list(list, sizeof(niri_window), parse_window, NULL, &items, &count))
        return false;
    free(state->windows);
    state->windows = items;
    state->window_count = count;

    state->focused_window = (opt_id){0};
    for (size_t i = 0; i < count; i++) {
        if (json_is_focused(json_at(list, i)))
            state->focused_window = (opt_id){.present = true, .value = state->windows[i].id};
    }
    return true;
}

static bool on_window_opened_or_changed(niri_state *state, const json_value *body) {
    const json_value *json = json_get(body, "window");
    niri_window parsed;
    if (!parse_window(json, NULL, &parsed))
        return false;

    niri_window *existing = find_window(state, parsed.id);
    if (existing) {
        *existing = parsed;
    } else {
        niri_window *windows = realloc(state->windows, (state->window_count + 1) * sizeof(*windows));
        if (!windows)
            return false;
        windows[state->window_count++] = parsed;
        state->windows = windows;
    }

    if (json_is_focused(json))
        state->focused_window = (opt_id){.present = true, .value = parsed.id};
    else if (opt_id_equal(state->focused_window, (opt_id){.present = true, .value = parsed.id}))
        state->focused_window = (opt_id){0};
    return true;
}

static bool on_window_closed(niri_state *state, const json_value *body) {
    uint64_t id;
    if (!json_as_u64(json_get(body, "id"), &id))
        return false;
    niri_window *window = find_window(state, id);
    if (window)
        *window = state->windows[--state->window_count];
    if (opt_id_equal(state->focused_window, (opt_id){.present = true, .value = id}))
        state->focused_window = (opt_id){0};
    return true;
}

static bool on_window_focus_changed(niri_state *state, const json_value *body) {
    const json_value *id = json_get(body, "id");
    if (json_has_kind(id, JSON_NULL)) {
        state->focused_window = (opt_id){0};
        return true;
    }
    opt_id focused = parse_opt_id(id);
    if (!focused.present)
        return false;
    state->focused_window = focused;
    return true;
}

static bool on_window_layouts_changed(niri_state *state, const json_value *body) {
    const json_value *changes = json_get(body, "changes");
    if (!json_has_kind(changes, JSON_ARRAY))
        return false;
    // Validate first so a bad entry leaves every window untouched
    for (size_t i = 0; i < json_count(changes); i++) {
        uint64_t id;
        if (!json_as_u64(json_at(json_at(changes, i), 0), &id))
            return false;
    }
    for (size_t i = 0; i < json_count(changes); i++) {
        const json_value *change = json_at(changes, i);
        uint64_t id;
        json_as_u64(json_at(change, 0), &id);
        // A layout for a window not seen yet is dropped; its own
        // WindowOpenedOrChanged will carry the layout again
        niri_window *window = find_window(state, id);
        if (window)
            window->tile = parse_tile_size(json_at(change, 1));
    }
    return true;
}

static bool on_overview_opened_or_closed(niri_state *state, const json_value *body) {
    return json_as_bool(json_get(body, "is_open"), &state->overview_open);
}

static const struct {
    const char *name;
    bool (*apply)(niri_state *state, const json_value *body);
} event_handlers[] = {
    {"WorkspacesChanged", on_workspaces_changed},
    {"WindowsChanged", on_windows_changed},
    {"WindowOpenedOrChanged", on_window_opened_or_changed},
    {"WindowClosed", on_window_closed},
    {"WindowFocusChanged", on_window_focus_changed},
    {"WindowLayoutsChanged", on_window_layouts_changed},
    {"OverviewOpenedOrClosed", on_overview_opened_or_closed},
};

niri_state *niri_state_new(void) {
    niri_state *state = calloc(1, sizeof(*state));
    if (state)
        state->outputs_stale = true;
    return state;
}

void niri_state_free(niri_state *state) {
    if (!state)
        return;
    free(state->windows);
    free_elements(state->workspaces, state->workspace_count, sizeof(niri_workspace), free_workspace);
    free_elements(state->outputs, state->output_count, sizeof(niri_output), free_output);
    free(state);
}

niri_event_result niri_state_apply_event(niri_state *state, const json_value *event) {
    // Events are externally tagged: {"Name": {fields...}}. A future variant
    // without fields would arrive as a bare string; it has no key, so it is
    // ignored along with every other event this tracker does not handle.
    const char *name = json_key_at(event, 0);
    if (!name)
        return NIRI_EVENT_OK;

    focus_geometry before = focused_geometry(state);
    bool ok = true;
    for (size_t i = 0; i < sizeof(event_handlers) / sizeof(event_handlers[0]); i++) {
        if (strcmp(name, event_handlers[i].name) == 0) {
            ok = event_handlers[i].apply(state, json_at(event, 0));
            break;
        }
    }
    if (!geometry_equal(before, focused_geometry(state)))
        state->outputs_stale = true;
    return ok ? NIRI_EVENT_OK : NIRI_EVENT_MALFORMED;
}

bool niri_state_outputs_stale(const niri_state *state) {
    return state->outputs_stale;
}

bool niri_state_set_outputs(niri_state *state, const json_value *outputs) {
    if (!json_has_kind(outputs, JSON_OBJECT))
        return false;
    void *items;
    size_t count;
    if (!parse_list(outputs, sizeof(niri_output), parse_output, free_output, &items, &count))
        return false;
    free_elements(state->outputs, state->output_count, sizeof(niri_output), free_output);
    state->outputs = items;
    state->output_count = count;
    state->outputs_stale = false;
    return true;
}

bool niri_state_should_pause(const niri_state *state) {
    if (state->overview_open)
        return false;
    const niri_window *window = focused_window(state);
    if (!window || !window->tile.present || !window->workspace_id.present)
        return false;
    const niri_workspace *workspace = find_workspace(state, window->workspace_id.value);
    if (!workspace || !workspace->output)
        return false;
    const niri_output *output = find_output(state, workspace->output);
    if (!output || !output->logical.present)
        return false;
    return window->tile.width + NIRI_COVER_TOLERANCE >= output->logical.width &&
           window->tile.height + NIRI_COVER_TOLERANCE >= output->logical.height;
}

// ==== Socket IO ====

// Upper bound for one reply or event line. WindowsChanged lists every
// window and stays far below this in practice.
#define NIRI_MAX_LINE (16 * 1024 * 1024)
#define NIRI_REQUEST_TIMEOUT_MS 2000
#define NIRI_RETRY_MIN_SECS 1
#define NIRI_RETRY_MAX_SECS 30

// poll() with cancellation enabled for its duration only, so the thread is
// never cancelled while holding allocations or logging.
static int wait_readable(int fd, int timeout_ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    int ready = poll(&pfd, 1, timeout_ms);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    return ready;
}

static void cancellable_sleep(unsigned secs) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    sleep(secs);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
}

static int connect_socket(const char *path) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

// Sends one request line and closes the write side; niri reads no further
// requests after EventStream, and a single request needs nothing more.
static bool send_request(int fd, const char *request) {
    char line[64];
    int len = snprintf(line, sizeof(line), "%s\n", request);
    if (len < 0 || (size_t)len >= sizeof(line))
        return false;
    for (int sent = 0; sent < len;) {
        // MSG_NOSIGNAL: a closed socket must not raise SIGPIPE
        ssize_t n = send(fd, line + sent, (size_t)(len - sent), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        sent += (int)n;
    }
    shutdown(fd, SHUT_WR);
    return true;
}

// Accumulates socket bytes and hands out complete newline-terminated lines.
typedef struct {
    char *data;
    size_t len, cap;
    size_t start; // offset of the first unconsumed byte
} line_buffer;

// Reads whatever is available. Returns bytes read, 0 at end of stream, or
// -1 on error, including a line that would exceed NIRI_MAX_LINE.
static ssize_t line_buffer_fill(line_buffer *b, int fd) {
    if (b->start > 0) {
        memmove(b->data, b->data + b->start, b->len - b->start);
        b->len -= b->start;
        b->start = 0;
    }
    if (b->cap - b->len < 4096) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        if (cap > NIRI_MAX_LINE)
            return -1;
        char *data = realloc(b->data, cap);
        if (!data)
            return -1;
        b->data = data;
        b->cap = cap;
    }
    ssize_t n;
    do {
        n = read(fd, b->data + b->len, b->cap - b->len);
    } while (n < 0 && errno == EINTR);
    if (n > 0)
        b->len += (size_t)n;
    return n;
}

static bool line_buffer_next(line_buffer *b, const char **line, size_t *len) {
    char *newline = b->start < b->len ? memchr(b->data + b->start, '\n', b->len - b->start) : NULL;
    if (!newline)
        return false;
    *line = b->data + b->start;
    *len = (size_t)(newline - *line);
    b->start = (size_t)(newline - b->data) + 1;
    return true;
}

// A request connection and its read buffer. Closed through a cleanup
// handler so a cancelled thread does not leak it.
typedef struct {
    int fd;
    line_buffer buffer;
} connection;

static bool connection_open(connection *c, const char *socket_path, const char *request) {
    *c = (connection){.fd = connect_socket(socket_path)};
    if (c->fd < 0)
        return false;
    if (!send_request(c->fd, request)) {
        close(c->fd);
        return false;
    }
    return true;
}

static void connection_close(void *arg) {
    connection *c = arg;
    free(c->buffer.data);
    close(c->fd);
}

static void state_cleanup(void *state) {
    niri_state_free(state);
}

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Sends a single request on a fresh connection and returns the parsed reply,
// or NULL on failure or timeout.
static json_value *niri_request(const char *socket_path, const char *request) {
    connection c;
    if (!connection_open(&c, socket_path, request))
        return NULL;

    json_value *reply = NULL;
    pthread_cleanup_push(connection_close, &c);
    long long deadline = monotonic_ms() + NIRI_REQUEST_TIMEOUT_MS;
    for (;;) {
        const char *line;
        size_t len;
        if (line_buffer_next(&c.buffer, &line, &len)) {
            reply = json_parse(line, len);
            break;
        }
        long long remaining = deadline - monotonic_ms();
        if (remaining <= 0 || wait_readable(c.fd, (int)remaining) <= 0 || line_buffer_fill(&c.buffer, c.fd) <= 0)
            break;
    }
    pthread_cleanup_pop(1);
    return reply;
}

static void refresh_outputs(const niri_monitor_config *config, niri_state *state) {
    json_value *reply = niri_request(config->socket_path, "\"Outputs\"");
    // Reply shape: {"Ok":{"Outputs":{"DP-1":{...}, ...}}}
    if (!niri_state_set_outputs(state, json_get(json_get(reply, "Ok"), "Outputs")) && config->verbose)
        cflp_warning("niri: failed to query outputs, will retry on the next change");
    json_free(reply);
}

// Remembers the last reported decision so on_change only sees changes.
// The consumer starts out assuming playback should not pause.
typedef struct {
    const niri_monitor_config *config;
    bool last;
} publisher;

static void publish(publisher *p, bool should_pause) {
    if (should_pause == p->last)
        return;
    p->last = should_pause;
    p->config->on_change(should_pause, p->config->userdata);
}

typedef enum {
    SESSION_UNREACHABLE, // could not connect or subscribe
    SESSION_DESYNCED,    // niri sent something this tracker cannot follow
    SESSION_CLOSED,      // the stream ended
} session_result;

// The first line after subscribing is the reply to EventStream.
static bool check_subscription_reply(const char *line, size_t len) {
    json_value *reply = json_parse(line, len);
    const char *ok = json_as_string(json_get(reply, "Ok"));
    bool handled = ok && strcmp(ok, "Handled") == 0;
    if (!handled) {
        const char *err = json_as_string(json_get(reply, "Err"));
        cflp_warning("niri: event stream refused: %s", err ? err : "unexpected reply");
    }
    json_free(reply);
    return handled;
}

// Applies one event line. Returns false when the tracked state can no
// longer be trusted and the session must resubscribe for a fresh snapshot.
static bool apply_event_line(niri_state *state, const char *line, size_t len) {
    json_value *event = json_parse(line, len);
    if (!event) {
        cflp_warning("niri: could not parse event, resubscribing");
        return false;
    }
    bool ok = niri_state_apply_event(state, event) == NIRI_EVENT_OK;
    if (!ok)
        cflp_warning("niri: unexpected %s event, resubscribing", json_key_at(event, 0));
    json_free(event);
    return ok;
}

// Follows one event stream connection until it fails. Starts from an empty
// state: niri opens every stream with a full snapshot.
static session_result run_session(const niri_monitor_config *config, publisher *pub) {
    connection c;
    if (!connection_open(&c, config->socket_path, "\"EventStream\""))
        return SESSION_UNREACHABLE;

    niri_state *state = niri_state_new();
    bool subscribed = false;
    bool healthy = state != NULL;
    pthread_cleanup_push(connection_close, &c);
    pthread_cleanup_push(state_cleanup, state);

    while (healthy && wait_readable(c.fd, -1) > 0 && line_buffer_fill(&c.buffer, c.fd) > 0) {
        const char *line;
        size_t len;
        while (healthy && line_buffer_next(&c.buffer, &line, &len)) {
            if (!subscribed) {
                healthy = subscribed = check_subscription_reply(line, len);
                if (subscribed && config->verbose)
                    cflp_info("niri: subscribed to event stream");
            } else {
                healthy = apply_event_line(state, line, len);
            }
        }
        // Decide once per batch so a burst of events (like the initial
        // snapshot) cannot toggle playback midway
        if (healthy && subscribed) {
            if (niri_state_outputs_stale(state))
                refresh_outputs(config, state);
            publish(pub, niri_state_should_pause(state));
        }
    }

    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);

    session_result result = !subscribed ? SESSION_UNREACHABLE : healthy ? SESSION_CLOSED : SESSION_DESYNCED;
    if (result == SESSION_CLOSED)
        cflp_warning("niri: event stream closed");
    return result;
}

void *niri_monitor_thread(void *arg) {
    const niri_monitor_config *config = arg;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    publisher pub = {.config = config, .last = false};
    unsigned retry_secs = NIRI_RETRY_MIN_SECS;
    bool warned_unreachable = false;

    for (;;) {
        session_result result = run_session(config, &pub);
        // Without a live stream nothing says the wallpaper is covered
        publish(&pub, false);

        if (result == SESSION_UNREACHABLE) {
            if (!warned_unreachable || config->verbose)
                cflp_warning("niri: cannot reach %s, retrying", config->socket_path);
            warned_unreachable = true;
        } else {
            warned_unreachable = false;
        }
        // A stream that merely closed is retried promptly; failing to connect
        // or to follow the stream backs off, so a niri version this tracker
        // cannot read costs one warning every NIRI_RETRY_MAX_SECS
        if (result == SESSION_CLOSED)
            retry_secs = NIRI_RETRY_MIN_SECS;
        cancellable_sleep(retry_secs);
        if (result != SESSION_CLOSED)
            retry_secs = retry_secs * 2 > NIRI_RETRY_MAX_SECS ? NIRI_RETRY_MAX_SECS : retry_secs * 2;
    }
    return NULL;
}
