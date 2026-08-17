// media_classify.c - exe name -> MediaProcessClass table + video-site title hints. All process knowledge lives here; media.c only consumes the classes.

#include "oled_aegis.h"
#include "media_classify.h"

// Case-insensitive substring search (MSVC _strnicmp-based)
static int ContainsIgnoreCase(const char* haystack, const char* needle)
{
    if (!haystack || !needle) return 0;

    size_t needleLen = strlen(needle);
    if (needleLen == 0) return 1;

    for (const char* p = haystack; *p; p++)
    {
        if (_strnicmp(p, needle, needleLen) == 0)
        {
            return 1;
        }
    }

    return 0;
}

// Exe names by class: audio-only apps are NOT video players (music must not keep the display on); Parsec maps as video; nvcontainer is background audio.
typedef struct
{
    const char* exeName;
    MediaProcessClass cls;
} ProcessClassEntry;

static const ProcessClassEntry g_processClasses[] = {
    { "chrome.exe",             MEDIA_CLASS_BROWSER },
    { "msedge.exe",             MEDIA_CLASS_BROWSER },
    { "firefox.exe",            MEDIA_CLASS_BROWSER },
    { "brave.exe",              MEDIA_CLASS_BROWSER },
    { "opera.exe",              MEDIA_CLASS_BROWSER },
    { "opera_gx.exe",           MEDIA_CLASS_BROWSER },
    { "vivaldi.exe",            MEDIA_CLASS_BROWSER },
    { "arc.exe",                MEDIA_CLASS_BROWSER },
    { "thorium.exe",            MEDIA_CLASS_BROWSER },
    { "zen.exe",                MEDIA_CLASS_BROWSER },

    { "vlc.exe",                MEDIA_CLASS_VIDEO_PLAYER },
    { "mpv.exe",                MEDIA_CLASS_VIDEO_PLAYER },
    { "mpvnet.exe",             MEDIA_CLASS_VIDEO_PLAYER },
    { "potplayer.exe",          MEDIA_CLASS_VIDEO_PLAYER },
    { "potplayermini.exe",      MEDIA_CLASS_VIDEO_PLAYER },
    { "potplayermini64.exe",    MEDIA_CLASS_VIDEO_PLAYER },
    { "wmplayer.exe",           MEDIA_CLASS_VIDEO_PLAYER },
    { "mpc-hc.exe",             MEDIA_CLASS_VIDEO_PLAYER },
    { "mpc-hc64.exe",           MEDIA_CLASS_VIDEO_PLAYER },
    { "mpc-be.exe",             MEDIA_CLASS_VIDEO_PLAYER },
    { "mpc-be64.exe",           MEDIA_CLASS_VIDEO_PLAYER },
    { "kodi.exe",               MEDIA_CLASS_VIDEO_PLAYER },
    { "plex.exe",               MEDIA_CLASS_VIDEO_PLAYER },
    { "jellyfinmediaplayer.exe",MEDIA_CLASS_VIDEO_PLAYER },
    { "embytheater.exe",        MEDIA_CLASS_VIDEO_PLAYER },
    { "video.ui.exe",           MEDIA_CLASS_VIDEO_PLAYER },
    { "parsec.exe",             MEDIA_CLASS_VIDEO_PLAYER },
    { "parsecd.exe",            MEDIA_CLASS_VIDEO_PLAYER },

    { "spotify.exe",            MEDIA_CLASS_AUDIO_ONLY },
    { "itunes.exe",             MEDIA_CLASS_AUDIO_ONLY },
    { "music.exe",              MEDIA_CLASS_AUDIO_ONLY },  // Apple Music for Windows
    { "foobar2000.exe",         MEDIA_CLASS_AUDIO_ONLY },
    { "winamp.exe",             MEDIA_CLASS_AUDIO_ONLY },
    { "musicbee.exe",           MEDIA_CLASS_AUDIO_ONLY },
    { "deezer.exe",             MEDIA_CLASS_AUDIO_ONLY },
    { "tidal.exe",              MEDIA_CLASS_AUDIO_ONLY },
    { "qobuz.exe",              MEDIA_CLASS_AUDIO_ONLY },
    { "amazon music.exe",       MEDIA_CLASS_AUDIO_ONLY },
    { "youtube music.exe",      MEDIA_CLASS_AUDIO_ONLY },
    { "groove music.exe",       MEDIA_CLASS_AUDIO_ONLY },
    { "mediamonkey.exe",        MEDIA_CLASS_AUDIO_ONLY },
    { "clementine.exe",         MEDIA_CLASS_AUDIO_ONLY },
    { "strawberry.exe",         MEDIA_CLASS_AUDIO_ONLY },
    { "audacious.exe",          MEDIA_CLASS_AUDIO_ONLY },

    { "nvcontainer.exe",        MEDIA_CLASS_BACKGROUND },
};

MediaProcessClass ClassifyProcess(const char* processName)
{
    if (!processName) return MEDIA_CLASS_UNKNOWN;

    for (int i = 0; i < (int)(sizeof(g_processClasses) / sizeof(g_processClasses[0])); i++)
    {
        if (_stricmp(processName, g_processClasses[i].exeName) == 0)
        {
            return g_processClasses[i].cls;
        }
    }
    return MEDIA_CLASS_UNKNOWN;
}

// Video-site title hints only (audio-only services excluded: music must not keep the display on; "YouTube Music" is covered by the "YouTube" hint).
static int WindowTitleHasMediaHint(const char* title)
{
    static const char* const mediaTitleHints[] = {
        "YouTube",
        "Twitch",
        "Netflix",
        "Hulu",
        "Disney+",
        "Prime Video",
        "Amazon Prime",
        "HBO Max",
        "Paramount+",
        "Peacock",
        "Crunchyroll",
        "Vimeo",
        "Dailymotion",
        "Plex",
        "Jellyfin",
        "Emby",
        "Media Player",
        "VLC media player",
        "Picture in picture",
        "TikTok",
        "/ X"           // x.com titles are "Home / X", "@user / X", "user on X: ... / X"
    };

    if (!title || title[0] == '\0') return 0;

    for (int i = 0; i < (int)(sizeof(mediaTitleHints) / sizeof(mediaTitleHints[0])); i++)
    {
        if (ContainsIgnoreCase(title, mediaTitleHints[i]))
        {
            return 1;
        }
    }

    return 0;
}

int WindowCountsAsMedia(const char* processName, const char* title)
{
    if (ClassifyProcess(processName) == MEDIA_CLASS_VIDEO_PLAYER)
    {
        return 1;
    }
    return WindowTitleHasMediaHint(title);
}
