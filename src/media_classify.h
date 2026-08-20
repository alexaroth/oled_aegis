// media_classify.h - exe name -> MediaProcessClass table; policy (geometry mapping, never-block classes) is expressed in terms of that class.

#ifndef MEDIA_CLASSIFY_H
#define MEDIA_CLASSIFY_H

typedef enum
{
    MEDIA_CLASS_UNKNOWN = 0,  // Anything else: audible + visible window = media on that monitor (mapped by geometry)
    MEDIA_CLASS_BROWSER,      // Multi-process browsers (audio matched by exe name)
    MEDIA_CLASS_VIDEO_PLAYER, // Video players and remote-streaming clients
    MEDIA_CLASS_AUDIO_ONLY,   // Music/podcast: audio never keeps a display on
    MEDIA_CLASS_BACKGROUND    // System/launcher/UI audio: never represents playback
} MediaProcessClass;

// Class of the exe owning an audio session or window; adding an app to the policy is a one-line entry in the table in media_classify.c.
// The table is a NEGATIVE list only (music/launcher/background noise that must never block) plus browser/player
// semantics - every other audible app with a visible window is mapped by geometry, so no per-app positive whitelist.
MediaProcessClass ClassifyProcess(const char* processName);

#endif // MEDIA_CLASSIFY_H
