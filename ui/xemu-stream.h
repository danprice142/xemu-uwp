/*
 * xemu network broadcast (video streaming)
 *
 * Captures the composed display frame and broadcasts H.264 to a configurable
 * URL (e.g. udp://host:port). Desktop FFmpeg builds and Xbox/UWP Media
 * Foundation builds both send an MPEG-TS program containing H.264 and audio.
 * Encoding runs on a dedicated worker thread so the render thread is never
 * blocked.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XEMU_STREAM_H
#define XEMU_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize / tear down the streaming subsystem. */
void xemu_stream_init(void);
void xemu_stream_shutdown(void);

/*
 * Reconcile the running encoder with the desired state. Must be called once per
 * UI frame from the UI thread. Handles auto-start when broadcast is enabled,
 * the remote-start control channel, and setting changes.
 */
void xemu_stream_poll(void);

/* True while the encoder worker is running. */
bool xemu_stream_is_active(void);

/* True after the encoders and MPEG-TS mux have initialized successfully. */
bool xemu_stream_is_ready(void);

/* True when the encoder is running and ready to accept another frame. A true
 * result claims the current cadence slot, so callers must invoke this only
 * when they are about to record the GPU copy. The presenter uses it to skip
 * extra readbacks when not streaming or before the next frame deadline. */
bool xemu_stream_wants_frame(void);

/* Manual control (UI button / remote command). */
void xemu_stream_request_start(void);
void xemu_stream_request_start_with_params(const char *url,
                                           int bitrate_kbps, int fps);
void xemu_stream_request_stop(void);

/* Returns the encoded dimensions once the active encoder is ready. These are
 * derived from the composed presenter output, preserving display scale mode
 * and aspect-ratio treatment. */
bool xemu_stream_get_output_size(int *width, int *height);

/* Human-readable status snapshot (thread-safe, static storage). */
const char *xemu_stream_get_status(void);

/* True if this build has streaming support compiled in (FFmpeg on desktop,
 * Media Foundation on UWP). */
bool xemu_stream_is_supported(void);

/*
 * Submit one composed BGRA8 frame (top-down, DXGI B8G8R8A8 byte order) captured
 * from the presenter backbuffer. The pixels are copied into an internal queue
 * and this returns immediately. No-op when the encoder is not running or the
 * queue is full (the frame is dropped). Also applies target-framerate gating.
 */
void xemu_stream_submit_bgra(const uint8_t *bgra, int width, int height,
                             int pitch);

/* Submit a persistently mapped frame without copying it on the render thread.
 * The storage must remain valid until release(opaque) is called. The callback
 * is invoked exactly once, including when streaming is inactive or the queue
 * is full. UWP's D3D12 presenter uses this to transfer a readback slot directly
 * to the encoder worker. capture_us is the monotonic time the frame was
 * captured (g_get_monotonic_time()), or 0 for the time of submission. */
typedef void (*XemuStreamFrameRelease)(void *opaque);
void xemu_stream_submit_bgra_borrowed(const uint8_t *bgra, int width,
                                      int height, int pitch,
                                      int64_t capture_us,
                                      XemuStreamFrameRelease release,
                                      void *opaque);

#ifdef __cplusplus
}
#endif

#endif
