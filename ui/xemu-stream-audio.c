/* Mixed-output audio capture bridge for the network streamer. */

#include "qemu/osdep.h"

#include "xemu-stream-audio.h"

#include <glib.h>

typedef struct StreamAudioCapture {
    XemuStreamAudioCallback callback;
    void *opaque;
    unsigned int callbacks_in_flight;
    bool stopping;
} StreamAudioCapture;

static StreamAudioCapture s_capture;
static GMutex s_capture_lock;
static GCond s_capture_cond;
static gint s_capture_enabled;

void xemu_stream_audio_push_pcm(const void *pcm, size_t size)
{
    StreamAudioCapture *capture = &s_capture;
    XemuStreamAudioCallback callback = NULL;
    void *callback_opaque = NULL;

    if (pcm == NULL || size == 0 || !g_atomic_int_get(&s_capture_enabled)) {
        return;
    }

    /* The platform mixer may invoke this while the UI tears the streamer
     * down. Take a counted callback reference so stop cannot return early,
     * but never wait on this emulation-critical audio path. */
    if (!g_mutex_trylock(&s_capture_lock)) {
        return;
    }
    if (!capture->stopping && capture->callback != NULL) {
        callback = capture->callback;
        callback_opaque = capture->opaque;
        capture->callbacks_in_flight++;
    }
    g_mutex_unlock(&s_capture_lock);

    if (callback != NULL) {
        callback(pcm, size, callback_opaque);

        g_mutex_lock(&s_capture_lock);
        capture->callbacks_in_flight--;
        if (capture->callbacks_in_flight == 0) {
            g_cond_broadcast(&s_capture_cond);
        }
        g_mutex_unlock(&s_capture_lock);
    }
}

bool xemu_stream_audio_start(XemuStreamAudioCallback callback, void *opaque)
{
    g_mutex_lock(&s_capture_lock);
    if (callback == NULL) {
        g_mutex_unlock(&s_capture_lock);
        return false;
    }
    s_capture.stopping = false;
    s_capture.callback = callback;
    s_capture.opaque = opaque;
    g_atomic_int_set(&s_capture_enabled, true);
    g_mutex_unlock(&s_capture_lock);
    return true;
}

void xemu_stream_audio_stop(void)
{
    g_atomic_int_set(&s_capture_enabled, false);
    g_mutex_lock(&s_capture_lock);
    s_capture.stopping = true;
    s_capture.callback = NULL;
    s_capture.opaque = NULL;
    while (s_capture.callbacks_in_flight != 0) {
        g_cond_wait(&s_capture_cond, &s_capture_lock);
    }
    s_capture.stopping = false;
    g_mutex_unlock(&s_capture_lock);
}
