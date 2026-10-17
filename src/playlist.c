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

char *playlist_next(const char *directory, const char *after, time_t now,
                    playlist_skip_fn skip, void *userdata) {
    DIR *dir = opendir(directory);
    if (!dir)
        return NULL;

    // One pass tracking two names: the smallest name after `after` (the
    // successor) and the smallest name overall (where the rotation wraps)
    char *successor = NULL;
    char *first = NULL;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!is_candidate(dirfd(dir), name, now, skip, userdata))
            continue;
        if (!first || strverscmp(name, first) < 0) {
            free(first);
            first = strdup(name);
        }
        if (after && strverscmp(name, after) > 0 && (!successor || strverscmp(name, successor) < 0)) {
            free(successor);
            successor = strdup(name);
        }
    }
    closedir(dir);

    if (successor) {
        free(first);
        return successor;
    }
    return first;
}
