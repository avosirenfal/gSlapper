#define _GNU_SOURCE // strverscmp
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "playlist.h"

static const char *const image_extensions[] = {".jpg", ".jpeg", ".png", ".webp", ".gif", NULL};
static const char *const video_extensions[] = {
    ".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".ogv", ".wmv", ".flv", ".mpg", ".mpeg", ".ts", NULL,
};

static bool has_extension(const char *name, const char *const *extensions) {
    const char *ext = name ? strrchr(name, '.') : NULL;
    if (!ext)
        return false;
    for (size_t i = 0; extensions[i]; i++) {
        if (strcasecmp(ext, extensions[i]) == 0)
            return true;
    }
    return false;
}

bool media_is_image_name(const char *name) {
    return has_extension(name, image_extensions);
}

bool media_is_video_name(const char *name) {
    return has_extension(name, video_extensions);
}

static bool is_candidate(int dir_fd, const char *name, time_t now, playlist_skip_fn skip, void *userdata) {
    if (name[0] == '.' || !(media_is_image_name(name) || media_is_video_name(name)))
        return false;
    // stat follows symlinks, so a link to a media file counts as one
    struct stat st;
    if (fstatat(dir_fd, name, &st, 0) != 0 || !S_ISREG(st.st_mode))
        return false;
    if (st.st_mtime > now - PLAYLIST_SETTLE_SECS)
        return false;
    return !(skip && skip(name, userdata));
}

// Calls visit for every candidate in one pass over the folder. Returns
// false when the folder cannot be read.
static bool for_each_candidate(const char *directory, time_t now, playlist_skip_fn skip, void *skip_data,
                               void (*visit)(const char *name, void *context), void *context) {
    DIR *dir = opendir(directory);
    if (!dir)
        return false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (is_candidate(dirfd(dir), entry->d_name, now, skip, skip_data))
            visit(entry->d_name, context);
    }
    closedir(dir);
    return true;
}

static void replace_name(char **slot, const char *name) {
    free(*slot);
    *slot = strdup(name);
}

// Tracks two names: the smallest name after `after` (the successor) and the
// smallest name overall (where the rotation wraps)
typedef struct {
    const char *after;
    char *successor;
    char *first;
} next_search;

static void visit_for_next(const char *name, void *context) {
    next_search *search = context;
    if (!search->first || strverscmp(name, search->first) < 0)
        replace_name(&search->first, name);
    if (search->after && strverscmp(name, search->after) > 0 &&
        (!search->successor || strverscmp(name, search->successor) < 0))
        replace_name(&search->successor, name);
}

char *playlist_next(const char *directory, const char *after, time_t now,
                    playlist_skip_fn skip, void *userdata) {
    next_search search = {.after = after};
    for_each_candidate(directory, now, skip, userdata, visit_for_next, &search);
    if (search.successor) {
        free(search.first);
        return search.successor;
    }
    return search.first;
}

// Reservoir sampling: the k-th candidate replaces the choice with
// probability 1/k, which leaves every candidate equally likely.
typedef struct {
    playlist_random_fn random;
    void *random_data;
    unsigned seen;
    char *choice;
} random_search;

static void visit_for_random(const char *name, void *context) {
    random_search *search = context;
    search->seen++;
    if (search->random(search->seen, search->random_data) == 0)
        replace_name(&search->choice, name);
}

char *playlist_random(const char *directory, time_t now, playlist_skip_fn skip, void *skip_data,
                      playlist_random_fn random, void *random_data) {
    random_search search = {.random = random, .random_data = random_data};
    for_each_candidate(directory, now, skip, skip_data, visit_for_random, &search);
    return search.choice;
}
