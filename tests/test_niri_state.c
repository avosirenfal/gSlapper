#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "niri.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static json_value *parse(const char *text) {
    return json_parse(text, strlen(text));
}

static niri_event_result apply(niri_state *state, const char *event_json) {
    json_value *event = parse(event_json);
    if (!event) {
        fprintf(stderr, "test event is not valid JSON: %s\n", event_json);
        failures++;
        return NIRI_EVENT_MALFORMED;
    }
    niri_event_result result = niri_state_apply_event(state, event);
    json_free(event);
    return result;
}

static bool set_outputs(niri_state *state, const char *outputs_json) {
    json_value *outputs = parse(outputs_json);
    bool ok = niri_state_set_outputs(state, outputs);
    json_free(outputs);
    return ok;
}

// ---- JSON reader ----

static void test_json(void) {
    json_value *v = parse(" {\"a\": [1, 2.5e1, -0.5, true, null], \"s\": \"x\\n\\u00e9\\ud83d\\ude00\"} ");
    CHECK(v != NULL);
    CHECK(json_count(json_get(v, "a")) == 5);
    uint64_t u;
    CHECK(json_as_u64(json_at(json_get(v, "a"), 0), &u) && u == 1);
    double d;
    CHECK(json_as_double(json_at(json_get(v, "a"), 1), &d) && d == 25.0);
    CHECK(!json_as_u64(json_at(json_get(v, "a"), 2), &u));
    bool b;
    CHECK(json_as_bool(json_at(json_get(v, "a"), 3), &b) && b);
    CHECK(json_has_kind(json_at(json_get(v, "a"), 4), JSON_NULL));
    CHECK(strcmp(json_as_string(json_get(v, "s")), "x\n\xc3\xa9\xf0\x9f\x98\x80") == 0);
    CHECK(json_get(v, "missing") == NULL);
    CHECK(json_get(json_get(v, "missing"), "deeper") == NULL);
    json_free(v);

    // Ids larger than a double can represent exactly
    v = parse("18446744073709551615");
    CHECK(json_as_u64(v, &u) && u == UINT64_MAX);
    json_free(v);
    v = parse("18446744073709551616");
    CHECK(!json_as_u64(v, &u));
    json_free(v);

    const char *invalid[] = {
        "", "{", "[1,]", "{\"a\":1,}", "01", "1.", "-", "\"unterminated", "\"\\x\"",
        "\"\\ud83d\"", "tru", "{} {}", "[1 2]", "{\"a\" 1}", "\"tab\there\"",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        v = parse(invalid[i]);
        if (v) {
            fprintf(stderr, "accepted invalid JSON: %s\n", invalid[i]);
            failures++;
            json_free(v);
        }
    }

    // Nesting beyond the depth limit is rejected rather than recursing on
    char deep[2 * 1000 + 1];
    memset(deep, '[', 1000);
    memset(deep + 1000, ']', 1000);
    deep[2000] = '\0';
    v = parse(deep);
    CHECK(v == NULL);
}

// ---- State tracking ----

// Mirrors niri's initial event-stream burst, including fields this tracker
// does not use, on two outputs.
static const char *SNAPSHOT_WORKSPACES =
    "{\"WorkspacesChanged\":{\"workspaces\":["
    "{\"id\":1,\"idx\":1,\"name\":null,\"output\":\"DP-1\",\"is_urgent\":false,\"is_active\":true,"
    "\"is_focused\":true,\"active_window_id\":10},"
    "{\"id\":2,\"idx\":1,\"name\":null,\"output\":\"HDMI-A-1\",\"is_urgent\":false,\"is_active\":true,"
    "\"is_focused\":false,\"active_window_id\":null}]}}";

static const char *SNAPSHOT_WINDOWS =
    "{\"WindowsChanged\":{\"windows\":["
    "{\"id\":10,\"title\":\"mpv\",\"app_id\":\"mpv\",\"pid\":42,\"workspace_id\":1,\"is_focused\":true,"
    "\"is_floating\":false,\"is_urgent\":false,\"layout\":{\"pos_in_scrolling_layout\":[1,1],"
    "\"tile_size\":[1920.0,1080.0],\"window_size\":[1920,1080],\"tile_pos_in_workspace_view\":[0.0,0.0],"
    "\"window_offset_in_tile\":[0.0,0.0]},\"focus_timestamp\":{\"secs\":5,\"nanos\":0}},"
    "{\"id\":11,\"title\":\"term\",\"app_id\":\"foot\",\"pid\":43,\"workspace_id\":1,\"is_focused\":false,"
    "\"is_floating\":false,\"is_urgent\":false,\"layout\":{\"pos_in_scrolling_layout\":[2,1],"
    "\"tile_size\":[960.0,1048.0],\"window_size\":[956,1044],\"tile_pos_in_workspace_view\":null,"
    "\"window_offset_in_tile\":[2.0,2.0]},\"focus_timestamp\":null}]}}";

static const char *OUTPUTS =
    "{\"DP-1\":{\"name\":\"DP-1\",\"make\":\"Dell\",\"model\":\"U2720Q\",\"serial\":null,"
    "\"physical_size\":[600,340],\"modes\":[],\"current_mode\":0,\"is_custom_mode\":false,"
    "\"vrr_supported\":false,\"vrr_enabled\":false,"
    "\"logical\":{\"x\":0,\"y\":0,\"width\":1920,\"height\":1080,\"scale\":2.0,\"transform\":\"Normal\"}},"
    "\"HDMI-A-1\":{\"name\":\"HDMI-A-1\",\"make\":\"LG\",\"model\":\"TV\",\"serial\":null,"
    "\"physical_size\":null,\"modes\":[],\"current_mode\":0,\"is_custom_mode\":false,"
    "\"vrr_supported\":false,\"vrr_enabled\":false,"
    "\"logical\":{\"x\":1920,\"y\":0,\"width\":2560,\"height\":1440,\"scale\":1.0,\"transform\":\"Normal\"}}}";

static niri_state *snapshot_state(void) {
    niri_state *state = niri_state_new();
    CHECK(niri_state_outputs_stale(state));
    CHECK(apply(state, SNAPSHOT_WORKSPACES) == NIRI_EVENT_OK);
    CHECK(apply(state, SNAPSHOT_WINDOWS) == NIRI_EVENT_OK);
    CHECK(apply(state, "{\"OverviewOpenedOrClosed\":{\"is_open\":false}}") == NIRI_EVENT_OK);
    CHECK(apply(state, "{\"ConfigLoaded\":{\"failed\":false}}") == NIRI_EVENT_OK);
    // Outputs are unknown until queried, so nothing can be fullscreen yet
    CHECK(niri_state_outputs_stale(state));
    CHECK(!niri_state_should_pause(state));
    CHECK(set_outputs(state, OUTPUTS));
    CHECK(!niri_state_outputs_stale(state));
    return state;
}

static void test_snapshot(void) {
    niri_state *state = snapshot_state();
    CHECK(niri_state_should_pause(state));
    niri_state_free(state);
}

static void test_focus(void) {
    niri_state *state = snapshot_state();

    // Focusing the tiled terminal resumes; focus moving to a layer surface too
    CHECK(apply(state, "{\"WindowFocusChanged\":{\"id\":11}}") == NIRI_EVENT_OK);
    CHECK(!niri_state_should_pause(state));
    CHECK(apply(state, "{\"WindowFocusChanged\":{\"id\":10}}") == NIRI_EVENT_OK);
    CHECK(niri_state_should_pause(state));
    CHECK(apply(state, "{\"WindowFocusChanged\":{\"id\":null}}") == NIRI_EVENT_OK);
    CHECK(!niri_state_should_pause(state));

    // A changed window that reports itself unfocused drops focus
    CHECK(apply(state, "{\"WindowFocusChanged\":{\"id\":10}}") == NIRI_EVENT_OK);
    CHECK(apply(state, "{\"WindowOpenedOrChanged\":{\"window\":{\"id\":10,\"workspace_id\":1,"
                       "\"is_focused\":false,\"layout\":{\"tile_size\":[1920.0,1080.0]}}}}") == NIRI_EVENT_OK);
    CHECK(!niri_state_should_pause(state));

    // Closing the focused fullscreen window resumes
    CHECK(apply(state, "{\"WindowFocusChanged\":{\"id\":10}}") == NIRI_EVENT_OK);
    CHECK(niri_state_should_pause(state));
    CHECK(apply(state, "{\"WindowClosed\":{\"id\":10}}") == NIRI_EVENT_OK);
    CHECK(!niri_state_should_pause(state));
    niri_state_free(state);
}

static void test_layout_changes(void) {
    niri_state *state = snapshot_state();

    // Leaving fullscreen shrinks the tile, which also marks outputs stale
    CHECK(apply(state, "{\"WindowLayoutsChanged\":{\"changes\":[[10,{\"tile_size\":[1280.0,720.0],"
                       "\"window_size\":[1280,720]}],[999,{\"tile_size\":[1.0,1.0]}]]}}") == NIRI_EVENT_OK);
    CHECK(niri_state_outputs_stale(state));
    CHECK(!niri_state_should_pause(state));
    CHECK(set_outputs(state, OUTPUTS));

    // Layout changes to unfocused windows do not require new outputs
    CHECK(apply(state, "{\"WindowLayoutsChanged\":{\"changes\":[[11,{\"tile_size\":[900.0,1048.0]}]]}}") ==
          NIRI_EVENT_OK);
    CHECK(!niri_state_outputs_stale(state));

    // The terminal goes fullscreen and takes focus
    CHECK(apply(state, "{\"WindowLayoutsChanged\":{\"changes\":[[11,{\"tile_size\":[1920.0,1080.0]}]]}}") ==
          NIRI_EVENT_OK);
    CHECK(apply(state, "{\"WindowFocusChanged\":{\"id\":11}}") == NIRI_EVENT_OK);
    CHECK(niri_state_should_pause(state));

    // Moved to the larger output, the same tile no longer covers it
    CHECK(apply(state, "{\"WindowOpenedOrChanged\":{\"window\":{\"id\":11,\"workspace_id\":2,"
                       "\"is_focused\":true,\"layout\":{\"tile_size\":[1920.0,1080.0]}}}}") == NIRI_EVENT_OK);
    CHECK(!niri_state_should_pause(state));
    CHECK(apply(state, "{\"WindowLayoutsChanged\":{\"changes\":[[11,{\"tile_size\":[2560.0,1440.0]}]]}}") ==
          NIRI_EVENT_OK);
    CHECK(niri_state_should_pause(state));
    niri_state_free(state);
}

static void test_new_window(void) {
    niri_state *state = snapshot_state();

    // niri may announce focus before the window itself
    CHECK(apply(state, "{\"WindowFocusChanged\":{\"id\":12}}") == NIRI_EVENT_OK);
    CHECK(!niri_state_should_pause(state));
    CHECK(apply(state, "{\"WindowOpenedOrChanged\":{\"window\":{\"id\":12,\"workspace_id\":2,"
                       "\"is_focused\":true,\"layout\":{\"tile_size\":[2560.0,1440.0]}}}}") == NIRI_EVENT_OK);
    CHECK(niri_state_should_pause(state));
    niri_state_free(state);
}

static void test_fractional_scale(void) {
    niri_state *state = snapshot_state();
    CHECK(set_outputs(state, "{\"DP-1\":{\"name\":\"DP-1\",\"logical\":{\"width\":1707,\"height\":960,"
                             "\"scale\":1.5}}}"));
    CHECK(apply(state, "{\"WindowLayoutsChanged\":{\"changes\":[[10,{\"tile_size\":[1706.6666666666667,"
                       "960.0]}]]}}") == NIRI_EVENT_OK);
    CHECK(niri_state_should_pause(state));
    niri_state_free(state);
}

static void test_overview(void) {
    niri_state *state = snapshot_state();
    // The overview shows the wallpaper behind the workspaces
    CHECK(apply(state, "{\"OverviewOpenedOrClosed\":{\"is_open\":true}}") == NIRI_EVENT_OK);
    CHECK(!niri_state_should_pause(state));
    CHECK(apply(state, "{\"OverviewOpenedOrClosed\":{\"is_open\":false}}") == NIRI_EVENT_OK);
    CHECK(niri_state_should_pause(state));
    niri_state_free(state);
}

static void test_outputs_disappear(void) {
    niri_state *state = snapshot_state();

    // A disabled output has no logical geometry
    CHECK(set_outputs(state, "{\"DP-1\":{\"name\":\"DP-1\",\"logical\":null}}"));
    CHECK(!niri_state_should_pause(state));

    // Every monitor disconnected: workspaces lose their output and niri
    // reports no outputs at all
    CHECK(apply(state, "{\"WorkspacesChanged\":{\"workspaces\":[{\"id\":1,\"output\":null},"
                       "{\"id\":2,\"output\":null}]}}") == NIRI_EVENT_OK);
    CHECK(niri_state_outputs_stale(state));
    CHECK(set_outputs(state, "{}"));
    CHECK(!niri_state_should_pause(state));

    // The monitor comes back
    CHECK(apply(state, SNAPSHOT_WORKSPACES) == NIRI_EVENT_OK);
    CHECK(set_outputs(state, OUTPUTS));
    CHECK(niri_state_should_pause(state));
    niri_state_free(state);
}

static void test_forward_compatibility(void) {
    niri_state *state = snapshot_state();

    // Events this tracker does not know, including hypothetical future ones
    CHECK(apply(state, "{\"KeyboardLayoutSwitched\":{\"idx\":1}}") == NIRI_EVENT_OK);
    CHECK(apply(state, "{\"SomeFutureEvent\":{\"anything\":[1,2,3]}}") == NIRI_EVENT_OK);
    CHECK(apply(state, "\"SomeFutureUnitEvent\"") == NIRI_EVENT_OK);
    // Unknown fields alongside known ones
    CHECK(apply(state, "{\"WindowFocusChanged\":{\"id\":10,\"cause\":\"keyboard\"}}") == NIRI_EVENT_OK);
    CHECK(niri_state_should_pause(state));

    // A known event missing what it needs is reported and changes nothing
    CHECK(apply(state, "{\"WindowsChanged\":{}}") == NIRI_EVENT_MALFORMED);
    CHECK(apply(state, "{\"WindowsChanged\":{\"windows\":[{\"title\":\"no id\"}]}}") == NIRI_EVENT_MALFORMED);
    CHECK(apply(state, "{\"WindowClosed\":{\"id\":\"10\"}}") == NIRI_EVENT_MALFORMED);
    CHECK(apply(state, "{\"WindowLayoutsChanged\":{\"changes\":[[11,{\"tile_size\":[1920.0,1080.0]}],"
                       "[\"bad\",{}]]}}") == NIRI_EVENT_MALFORMED);
    CHECK(apply(state, "{\"WindowFocusChanged\":{\"id\":11}}") == NIRI_EVENT_OK);
    // The rejected layout batch did not make the terminal fullscreen
    CHECK(!niri_state_should_pause(state));
    CHECK(apply(state, "{\"WindowFocusChanged\":{\"id\":10}}") == NIRI_EVENT_OK);
    CHECK(niri_state_should_pause(state));

    // A malformed Outputs reply leaves the previous outputs in place
    CHECK(!set_outputs(state, "[]"));
    CHECK(!set_outputs(state, "{\"DP-1\":5}"));
    CHECK(niri_state_should_pause(state));
    niri_state_free(state);
}

int main(void) {
    test_json();
    test_snapshot();
    test_focus();
    test_layout_changes();
    test_new_window();
    test_fractional_scale();
    test_overview();
    test_outputs_disappear();
    test_forward_compatibility();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("niri state tests passed\n");
    return EXIT_SUCCESS;
}
