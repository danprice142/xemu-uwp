/*
 * Mixed-output audio capture bridge for the network streamer.
 *
 * Kept in C because QEMU's audio headers use C-only constructs and cannot be
 * included directly by the UWP C++ Media Foundation backend.
 */

#ifndef XEMU_STREAM_AUDIO_H
#define XEMU_STREAM_AUDIO_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*XemuStreamAudioCallback)(const void *pcm, size_t size,
                                        void *opaque);

/* Registers the network stream consumer for the Xbox APU's final mixed output,
 * which is 48-kHz stereo signed 16-bit little-endian PCM. */
bool xemu_stream_audio_start(XemuStreamAudioCallback callback, void *opaque);
void xemu_stream_audio_stop(void);

/* Called by the MCPX APU monitor immediately before the same PCM frame is sent
 * to the platform playback backend. */
void xemu_stream_audio_push_pcm(const void *pcm, size_t size);

#ifdef __cplusplus
}
#endif

#endif
