#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <stdbool.h>
#include <time.h>

// Folder playlists for the slideshow.
//
// The folder is rescanned every time the next item is chosen, and the
// position is remembered by file name rather than by index. Files can be
// added, renamed or deleted at any time: the next pick simply sees the
// folder as it is then.

// True for file names with a supported image or video extension.
bool media_is_image_name(const char *name);
bool media_is_video_name(const char *name);

// Decides whether a candidate should be passed over (for example because it
// already failed to load). Receives the bare file name.
typedef bool (*playlist_skip_fn)(const char *name, void *userdata);

// A file modified more recently than this is assumed to still be copying
// into the folder and is passed over until it settles.
#define PLAYLIST_SETTLE_SECS 5

// Returns the media file that follows `after` in natural sort order
// ("clip2" before "clip10"), wrapping around to the first one. With `after`
// NULL, returns the first file. Hidden files, non-media files, files that
// are not regular files and files modified after now - PLAYLIST_SETTLE_SECS
// are ignored, as are names `skip` rejects. `after` itself is a valid
// answer when it is the only candidate.
//
// Returns a malloc'd file name (not a path), or NULL when the folder has no
// candidates or cannot be read.
char *playlist_next(const char *directory, const char *after, time_t now,
                    playlist_skip_fn skip, void *userdata);

#endif // PLAYLIST_H
