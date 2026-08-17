// media_classify.h - exe name -> MediaProcessClass table; policy (window mapping, fallback skips) is expressed in terms of that class.

#ifndef MEDIA_CLASSIFY_H
#define MEDIA_CLASSIFY_H

typedef enum
{
    MEDIA_CLASS_UNKNOWN = 0,  // Anything else: fallback policy is conservative
    MEDIA_CLASS_BROWSER,      // Multi-process browsers (audio matched by exe name)
    MEDIA_CLASS_VIDEO_PLAYER, // Video players and remote-streaming clients
    MEDIA_CLASS_AUDIO_ONLY,   // Music/podcast: audio never keeps a display on
    MEDIA_CLASS_BACKGROUND    // System/background audio: never represents playback
} MediaProcessClass;

// Class of the exe owning an audio session or window; adding an app to the policy is a one-line entry in the table in media_classify.c.
MediaProcessClass ClassifyProcess(const char* processName);

// 1 if (process, title) looks media-playing: video players always count, others only with a video-site hint (browser hint is diagnostic only).
int WindowCountsAsMedia(const char* processName, const char* title);

#endif // MEDIA_CLASSIFY_H
