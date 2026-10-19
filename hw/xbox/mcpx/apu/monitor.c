/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_int.h"
#include "qemu/error-report.h"
#include "ui/xemu-stream-audio.h"

#ifdef CONFIG_UWP
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

#define MCPX_UWP_XAUDIO2_BUFFERS 8

typedef struct MCPXAPUXAudio2Monitor {
    IXAudio2 *engine;
    IXAudio2MasteringVoice *mastering_voice;
    IXAudio2SourceVoice *source_voice;
    int16_t buffers[MCPX_UWP_XAUDIO2_BUFFERS][256][2];
    unsigned next_buffer;
    bool warned_submit_failure;
} MCPXAPUXAudio2Monitor;

#if defined(XEMU_UWP_AUDIO_TRACE)
#define MCPX_MONITOR_UWP_TRACE(step) \
    fprintf(stderr, "xemu UWP: mcpx_apu_monitor_init " step "\n")
#else
#define MCPX_MONITOR_UWP_TRACE(step) do { } while (0)
#endif
#else
#define MCPX_MONITOR_UWP_TRACE(step) do { } while (0)
#endif

#ifdef CONFIG_UWP
static void mcpx_apu_monitor_xaudio2_destroy(MCPXAPUXAudio2Monitor *backend)
{
    if (!backend) {
        return;
    }

    if (backend->source_voice) {
        IXAudio2SourceVoice_Stop(backend->source_voice, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(backend->source_voice);
        IXAudio2SourceVoice_DestroyVoice(backend->source_voice);
    }
    if (backend->mastering_voice) {
        IXAudio2MasteringVoice_DestroyVoice(backend->mastering_voice);
    }
    if (backend->engine) {
        IXAudio2_StopEngine(backend->engine);
        IXAudio2_Release(backend->engine);
    }
    g_free(backend);
}

static bool mcpx_apu_monitor_xaudio2_init(MCPXAPUState *d, Error **errp)
{
    HRESULT hr;
    WAVEFORMATEX format = {
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = 2,
        .nSamplesPerSec = 48000,
        .nAvgBytesPerSec = 48000 * 2 * sizeof(int16_t),
        .nBlockAlign = 2 * sizeof(int16_t),
        .wBitsPerSample = 16,
        .cbSize = 0,
    };
    MCPXAPUXAudio2Monitor *backend = g_new0(MCPXAPUXAudio2Monitor, 1);

    MCPX_MONITOR_UWP_TRACE("creating XAudio2 engine");
    hr = XAudio2Create(&backend->engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        error_setg(errp, "XAudio2Create failed: HRESULT 0x%08lx",
                   (unsigned long)hr);
        goto fail;
    }

    MCPX_MONITOR_UWP_TRACE("creating XAudio2 mastering voice");
    hr = IXAudio2_CreateMasteringVoice(
        backend->engine, &backend->mastering_voice, XAUDIO2_DEFAULT_CHANNELS,
        XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL, AudioCategory_GameEffects);
    if (FAILED(hr)) {
        error_setg(errp, "IXAudio2_CreateMasteringVoice failed: HRESULT 0x%08lx",
                   (unsigned long)hr);
        goto fail;
    }

    MCPX_MONITOR_UWP_TRACE("creating XAudio2 source voice");
    hr = IXAudio2_CreateSourceVoice(
        backend->engine, &backend->source_voice, &format, 0,
        XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        error_setg(errp, "IXAudio2_CreateSourceVoice failed: HRESULT 0x%08lx",
                   (unsigned long)hr);
        goto fail;
    }

    MCPX_MONITOR_UWP_TRACE("starting XAudio2 source voice");
    hr = IXAudio2SourceVoice_Start(backend->source_voice, 0,
                                   XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        error_setg(errp, "IXAudio2SourceVoice_Start failed: HRESULT 0x%08lx",
                   (unsigned long)hr);
        goto fail;
    }

    d->monitor.uwp_backend = backend;
    d->monitor.queued_bytes_low = sizeof(d->monitor.frame_buf);
    d->monitor.queued_bytes_high =
        (MCPX_UWP_XAUDIO2_BUFFERS - 1) * sizeof(d->monitor.frame_buf);
    MCPX_MONITOR_UWP_TRACE("finished XAudio2 monitor backend");
    return true;

fail:
    mcpx_apu_monitor_xaudio2_destroy(backend);
    return false;
}

static void mcpx_apu_monitor_xaudio2_frame(MCPXAPUState *d)
{
    MCPXAPUXAudio2Monitor *backend = d->monitor.uwp_backend;
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER buffer = { 0 };
    HRESULT hr;

    if (!backend || !backend->source_voice) {
        return;
    }

    IXAudio2SourceVoice_GetState(backend->source_voice, &state, 0);
    if (state.BuffersQueued >= MCPX_UWP_XAUDIO2_BUFFERS) {
        return;
    }

    float volume = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
    IXAudio2SourceVoice_SetVolume(backend->source_voice, volume,
                                  XAUDIO2_COMMIT_NOW);

    unsigned buffer_index = backend->next_buffer++ % MCPX_UWP_XAUDIO2_BUFFERS;
    memcpy(backend->buffers[buffer_index], d->monitor.frame_buf,
           sizeof(d->monitor.frame_buf));

    buffer.AudioBytes = sizeof(backend->buffers[buffer_index]);
    buffer.pAudioData = (const BYTE *)backend->buffers[buffer_index];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(backend->source_voice, &buffer,
                                                NULL);
    if (FAILED(hr) && !backend->warned_submit_failure) {
        backend->warned_submit_failure = true;
        warn_report("XAudio2 SubmitSourceBuffer failed: HRESULT 0x%08lx",
                    (unsigned long)hr);
    }
}
#endif

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    d->monitor.stream = NULL;
    d->monitor.uwp_backend = NULL;

#ifdef CONFIG_UWP
    mcpx_apu_monitor_xaudio2_init(d, errp);
    return;
#else
    SDL_AudioSpec spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = 2,
    };

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        error_setg(errp, "SDL_Init failed: %s", SDL_GetError());
        return;
    }

    MCPX_MONITOR_UWP_TRACE("opening SDL default playback audio stream");
    d->monitor.stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (d->monitor.stream == NULL) {
        error_setg(errp, "SDL_OpenAudioDeviceStream failed: %s",
                   SDL_GetError());
        return;
    }
    MCPX_MONITOR_UWP_TRACE("opened SDL default playback audio stream");

    MCPX_MONITOR_UWP_TRACE("getting SDL audio stream device");
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(d->monitor.stream);
    MCPX_MONITOR_UWP_TRACE("got SDL audio stream device");

    SDL_AudioSpec dev_spec;
    int dev_buf_frames = 0;
    int dev_drain_bytes = 0;
    MCPX_MONITOR_UWP_TRACE("querying SDL audio device format");
    if (SDL_GetAudioDeviceFormat(dev, &dev_spec, &dev_buf_frames)) {
        dev_drain_bytes = dev_buf_frames * spec.channels *
                          SDL_AUDIO_BYTESIZE(spec.format) *
                          spec.freq / dev_spec.freq;
    }
    MCPX_MONITOR_UWP_TRACE("finished SDL audio device format query");
    int frame_bytes = sizeof(d->monitor.frame_buf);
    int drain = MAX(dev_drain_bytes, frame_bytes);
    d->monitor.queued_bytes_low = drain;
    d->monitor.queued_bytes_high = 3 * drain;

    SDL_ResumeAudioDevice(dev);
#endif
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
#ifdef CONFIG_UWP
    mcpx_apu_monitor_xaudio2_destroy(d->monitor.uwp_backend);
    d->monitor.uwp_backend = NULL;
#else
    if (d->monitor.stream) {
        SDL_DestroyAudioStream(d->monitor.stream);
    }
#endif
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    /*
     * Push the final stereo mix to the stream capture path before the
     * playback backend consumes it.  The monitor bypasses the QEMU audio
     * layer, so generic audio capture never sees these samples.
     */
    xemu_stream_audio_push_pcm(d->monitor.frame_buf,
                               sizeof(d->monitor.frame_buf));

#ifdef CONFIG_UWP
    mcpx_apu_monitor_xaudio2_frame(d);
#else
    if (d->monitor.stream) {
        float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
        SDL_SetAudioStreamGain(d->monitor.stream, vu);
        SDL_PutAudioStreamData(d->monitor.stream, d->monitor.frame_buf,
                            sizeof(d->monitor.frame_buf));
    }
#endif

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}
