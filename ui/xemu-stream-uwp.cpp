/*
 * Xbox/UWP network audio/video broadcast backend.
 *
 * Uses the inbox Media Foundation H.264 transform instead of loading a desktop
 * FFmpeg distribution into the AppContainer. Media Foundation supplies H.264
 * and AAC encoders; a small in-process muxer writes standards-compliant
 * MPEG-TS into UDP datagrams that common network media players can consume.
 * Frame capture remains D3D12-only and is performed by d3d12-presenter.cc.
 */

#include "qemu/osdep.h"

#include "ui/xemu-stream.h"
#include "ui/xemu-stream-audio.h"
#include "ui/xemu-settings.h"

#include <glib.h>

#include <cmath>
#include <mfapi.h>
#include <mferror.h>
/* mingw-w64 13.0 accidentally hides this Media Foundation type whenever
 * windows.h has already supplied the GDI bitmap declarations. */
#if defined(__MINGW32__) && defined(_WINGDI_)
typedef struct {
    GUID guidMajorType;
    GUID guidSubtype;
} MFT_REGISTER_TYPE_INFO;
#endif
#include <mftransform.h>
#include <mfidl.h>
#include <codecapi.h>

/* The codec settings interface, declared here because strmif.h only builds
 * for the desktop API family. Method order matches the Windows SDK. */
struct XemuCodecApi : public IUnknown {
    virtual HRESULT WINAPI IsSupported(const GUID *api) = 0;
    virtual HRESULT WINAPI IsModifiable(const GUID *api) = 0;
    virtual HRESULT WINAPI GetParameterRange(const GUID *api, VARIANT *min,
                                             VARIANT *max,
                                             VARIANT *step) = 0;
    virtual HRESULT WINAPI GetParameterValues(const GUID *api,
                                              VARIANT **values,
                                              ULONG *count) = 0;
    virtual HRESULT WINAPI GetDefaultValue(const GUID *api,
                                           VARIANT *value) = 0;
    virtual HRESULT WINAPI GetValue(const GUID *api, VARIANT *value) = 0;
    virtual HRESULT WINAPI SetValue(const GUID *api, VARIANT *value) = 0;
};
#include <mfreadwrite.h>
/* mingw-w64 currently hides this UWP-supported API and its flag enum behind
 * WINAPI_PARTITION_DESKTOP. Keep the declaration local until its headers match
 * the Windows SDK contract. */
EXTERN_C HRESULT WINAPI MFTEnumEx(
    GUID category, UINT32 flags, const MFT_REGISTER_TYPE_INFO *input_type,
    const MFT_REGISTER_TYPE_INFO *output_type, IMFActivate ***activations,
    UINT32 *activation_count);
static constexpr UINT32 XEMU_MFT_ENUM_FLAG_SYNCMFT = 0x00000001;
static constexpr UINT32 XEMU_MFT_ENUM_FLAG_ASYNCMFT = 0x00000002;
static constexpr UINT32 XEMU_MFT_ENUM_FLAG_HARDWARE = 0x00000004;
static constexpr UINT32 XEMU_MFT_ENUM_FLAG_LOCALMFT = 0x00000010;
static constexpr UINT32 XEMU_MFT_ENUM_FLAG_TRANSCODE_ONLY = 0x00000020;
static constexpr UINT32 XEMU_MFT_ENUM_FLAG_SORTANDFILTER = 0x00000040;
EXTERN_C const CLSID CLSID_CMSH264EncoderMFT;
EXTERN_C const CLSID CLSID_AACMFTEncoder;
/* IMFTrackedSample and its factory are supported in UWP, but mingw-w64 hides
 * the interface definition behind WINAPI_PARTITION_DESKTOP. Declare the
 * small COM contract locally instead of including evr.h, which also pulls in
 * desktop-only DirectShow declarations that do not compile for AppContainer. */
#ifndef __IMFTrackedSample_INTERFACE_DEFINED__
#define __IMFTrackedSample_INTERFACE_DEFINED__
MIDL_INTERFACE("245bf8e9-0755-40f7-88a5-ae0f18d55e17")
IMFTrackedSample : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetAllocator(
        IMFAsyncCallback *sample_allocator, IUnknown *state) = 0;
};
#endif
static const GUID XEMU_IID_IMFTrackedSample = {
    0x245bf8e9,
    0x0755,
    0x40f7,
    { 0x88, 0xa5, 0xae, 0x0f, 0x18, 0xd5, 0x5e, 0x17 }
};
EXTERN_C HRESULT WINAPI MFCreateTrackedSample(IMFTrackedSample **sample);
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define STREAM_MAX_OUTPUT_WIDTH 1280
#define STREAM_MAX_OUTPUT_HEIGHT 720
#define STREAM_QUEUE_MAX 1
#define STREAM_FRAME_POOL_MAX (STREAM_QUEUE_MAX + 1)
#define STREAM_VIDEO_INPUT_POOL_MAX (STREAM_QUEUE_MAX + 2)
#define STREAM_UDP_PAYLOAD (188 * 7)
#define STREAM_AUDIO_RATE 48000
#define STREAM_AUDIO_CHANNELS 2
#define STREAM_AUDIO_SAMPLES 1024
#define STREAM_AUDIO_BYTES_PER_FRAME (STREAM_AUDIO_CHANNELS * 2)
#define STREAM_AUDIO_QUEUE_MAX_BYTES \
    (STREAM_AUDIO_RATE * STREAM_AUDIO_BYTES_PER_FRAME * 2)
#define STREAM_AUDIO_POOL_MAX 64
#define STREAM_AUDIO_BLOCK_HINT 1024

struct StreamFrame {
    uint8_t *data;
    uint8_t *owned_data;
    size_t capacity;
    int width;
    int height;
    int pitch;
    int64_t timestamp_us;
    XemuStreamFrameRelease release;
    void *release_opaque;
};

struct StreamAudio {
    uint8_t *data;
    size_t size;
    size_t capacity;
    /* Monotonic time the block left the audio mixer. */
    int64_t arrival_us;
};

struct StreamParams {
    char url[512];
    int fps;
    int bitrate_kbps;
};

static GMutex s_lock;
static GCond s_cond;
static GQueue *s_queue;
static GQueue *s_frame_pool;
static GQueue *s_audio_queue;
static GQueue *s_audio_pool;
static size_t s_audio_queue_bytes;
static bool s_audio_capture_started;
static GThread *s_worker;
static std::atomic_bool s_worker_run{false};
static std::atomic_bool s_active{false};
static std::atomic_bool s_ready{false};
static std::atomic_bool s_start_failed{false};
static std::atomic_bool s_remote_active{false};
static bool s_remote_override;
static StreamParams s_remote_params;
static GThread *s_control;
static std::atomic_bool s_control_run{false};
static int s_control_port_running;
static StreamParams s_running_params;
static char s_status[256];
static int64_t s_next_capture_us;
static int64_t s_frame_interval_us;
static int s_output_width;
static int s_output_height;
static std::atomic_bool s_initialized{false};
static bool s_sync_initialized;

static void set_status(const char *fmt, ...)
{
    char status[sizeof(s_status)];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);

    g_mutex_lock(&s_lock);
    g_strlcpy(s_status, status, sizeof(s_status));
    g_mutex_unlock(&s_lock);

    /* LocalState stderr can be backed by comparatively slow storage on Xbox.
     * Never hold the stream queue lock while writing or flushing it. */
    fprintf(stderr, "xemu stream: %s\n", status);
    fflush(stderr);
}

static void stream_frame_free(StreamFrame *f)
{
    if (f != nullptr) {
        if (f->release != nullptr) {
            f->release(f->release_opaque);
        }
        g_free(f->owned_data);
        g_free(f);
    }
}

static StreamFrame *stream_frame_acquire(size_t size)
{
    StreamFrame *frame = nullptr;

    g_mutex_lock(&s_lock);
    if (s_frame_pool != nullptr) {
        frame = static_cast<StreamFrame *>(g_queue_pop_head(s_frame_pool));
    }
    g_mutex_unlock(&s_lock);

    if (frame == nullptr) {
        frame = g_new0(StreamFrame, 1);
    }
    if (frame->capacity != size) {
        frame->owned_data = static_cast<uint8_t *>(
            g_realloc(frame->owned_data, size));
        frame->capacity = size;
    }
    frame->data = frame->owned_data;
    frame->release = nullptr;
    frame->release_opaque = nullptr;
    return frame;
}

static StreamFrame *stream_frame_acquire_borrowed(
    const uint8_t *data, XemuStreamFrameRelease release, void *opaque)
{
    StreamFrame *frame = nullptr;

    g_mutex_lock(&s_lock);
    if (s_frame_pool != nullptr) {
        frame = static_cast<StreamFrame *>(g_queue_pop_head(s_frame_pool));
    }
    g_mutex_unlock(&s_lock);
    if (frame == nullptr) {
        frame = g_new0(StreamFrame, 1);
    }
    frame->data = const_cast<uint8_t *>(data);
    frame->release = release;
    frame->release_opaque = opaque;
    return frame;
}

static void stream_frame_recycle_locked(StreamFrame *frame)
{
    if (frame == nullptr) {
        return;
    }
    if (frame->release != nullptr) {
        frame->release(frame->release_opaque);
    }
    frame->data = frame->owned_data;
    frame->release = nullptr;
    frame->release_opaque = nullptr;
    frame->width = 0;
    frame->height = 0;
    frame->pitch = 0;
    frame->timestamp_us = 0;
    if (s_frame_pool != nullptr &&
        g_queue_get_length(s_frame_pool) < STREAM_FRAME_POOL_MAX) {
        g_queue_push_tail(s_frame_pool, frame);
    } else {
        stream_frame_free(frame);
    }
}

static void stream_frame_recycle(StreamFrame *frame)
{
    g_mutex_lock(&s_lock);
    stream_frame_recycle_locked(frame);
    g_mutex_unlock(&s_lock);
}

static void stream_audio_free(StreamAudio *audio)
{
    if (audio != nullptr) {
        g_free(audio->data);
        g_free(audio);
    }
}

static void stream_audio_recycle_locked(StreamAudio *audio)
{
    if (audio == nullptr) {
        return;
    }
    audio->size = 0;
    if (s_audio_pool != nullptr &&
        g_queue_get_length(s_audio_pool) < STREAM_AUDIO_POOL_MAX) {
        g_queue_push_tail(s_audio_pool, audio);
    } else {
        stream_audio_free(audio);
    }
}

static void stream_audio_recycle(StreamAudio *audio)
{
    g_mutex_lock(&s_lock);
    stream_audio_recycle_locked(audio);
    g_mutex_unlock(&s_lock);
}

static void audio_capture_pcm(const void *buf, size_t size, void *)
{
    if (buf == nullptr || size == 0 ||
        !s_initialized.load(std::memory_order_acquire) ||
        !s_active.load(std::memory_order_acquire)) {
        return;
    }
    /* Never let the software encoder or its queues block MCPX audio. All
     * normal monitor blocks fit the preallocated pool; under contention or
     * sustained encoder backlog, dropping one block is preferable to pausing
     * the entire emulated machine. */
    if (!g_mutex_trylock(&s_lock)) {
        return;
    }
    if (!s_active.load(std::memory_order_relaxed) ||
        s_audio_queue == nullptr || s_audio_pool == nullptr) {
        g_mutex_unlock(&s_lock);
        return;
    }
    auto *audio = static_cast<StreamAudio *>(
        g_queue_pop_head(s_audio_pool));
    if (audio == nullptr || audio->capacity < size) {
        stream_audio_recycle_locked(audio);
        g_mutex_unlock(&s_lock);
        return;
    }
    audio->size = size;
    audio->arrival_us = g_get_monotonic_time();
    memcpy(audio->data, buf, size);
    while (!g_queue_is_empty(s_audio_queue) &&
           s_audio_queue_bytes + audio->size >
               STREAM_AUDIO_QUEUE_MAX_BYTES) {
        auto *old = static_cast<StreamAudio *>(
            g_queue_pop_head(s_audio_queue));
        s_audio_queue_bytes -= old->size;
        stream_audio_recycle_locked(old);
    }
    if (audio->size <= STREAM_AUDIO_QUEUE_MAX_BYTES) {
        g_queue_push_tail(s_audio_queue, audio);
        s_audio_queue_bytes += audio->size;
        g_cond_signal(&s_cond);
        audio = nullptr;
    }
    stream_audio_recycle_locked(audio);
    g_mutex_unlock(&s_lock);
}

static void ensure_audio_capture(void)
{
    if (s_audio_capture_started) {
        return;
    }
    s_audio_capture_started =
        xemu_stream_audio_start(audio_capture_pcm, nullptr);
}

static void snapshot_config_params(StreamParams *out)
{
    const char *url = g_config.display.stream.url;
    if (url == nullptr || url[0] == '\0') {
        url = "udp://192.168.1.22:26960";
    }
    g_strlcpy(out->url, url, sizeof(out->url));
    out->fps = (g_config.display.stream.fps >= 1 &&
                g_config.display.stream.fps <= 120)
                   ? g_config.display.stream.fps
                   : 30;
    out->bitrate_kbps = (g_config.display.stream.bitrate_kbps >= 100 &&
                         g_config.display.stream.bitrate_kbps <= 100000)
                            ? g_config.display.stream.bitrate_kbps
                            : 6000;
}

static void snapshot_params(StreamParams *out)
{
    snapshot_config_params(out);

    /* A remote request publishes its override before setting active. Avoid
     * taking the queue lock for the normal config-only path, then recheck
     * under the lock so a concurrent STOP cannot leave a newly started worker
     * with a stale override. */
    if (!s_remote_active.load(std::memory_order_acquire)) {
        return;
    }
    g_mutex_lock(&s_lock);
    if (s_remote_active.load(std::memory_order_relaxed) &&
        s_remote_override) {
        *out = s_remote_params;
    }
    g_mutex_unlock(&s_lock);
}

static bool params_differ(const StreamParams &a, const StreamParams &b)
{
    return strcmp(a.url, b.url) != 0 || a.fps != b.fps ||
           a.bitrate_kbps != b.bitrate_kbps;
}

static void compute_output_size(int src_w, int src_h, int *out_w, int *out_h)
{
    double scale = std::min((double)STREAM_MAX_OUTPUT_WIDTH / src_w,
                            (double)STREAM_MAX_OUTPUT_HEIGHT / src_h);
    scale = std::min(scale, 1.0);
    *out_w = std::max(16, ((int)(src_w * scale)) & ~1);
    *out_h = std::max(16, ((int)(src_h * scale)) & ~1);
}

static HRESULT hresult_from_wsa(void)
{
    return HRESULT_FROM_WIN32((DWORD)WSAGetLastError());
}

class UdpByteStream final : public IMFByteStream {
public:
    static HRESULT Create(const char *url, int bitrate_kbps,
                          UdpByteStream **out)
    {
        *out = nullptr;
        std::string value(url != nullptr ? url : "");
        constexpr const char prefix[] = "udp://";
        if (value.rfind(prefix, 0) != 0) {
            return E_INVALIDARG;
        }
        value.erase(0, sizeof(prefix) - 1);
        size_t query = value.find('?');
        if (query != std::string::npos) {
            value.resize(query);
        }
        size_t colon = value.rfind(':');
        if (colon == std::string::npos || colon == 0 ||
            colon + 1 >= value.size()) {
            return E_INVALIDARG;
        }

        std::string host = value.substr(0, colon);
        std::string port = value.substr(colon + 1);
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo *result = nullptr;
        int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
        if (rc != 0 || result == nullptr) {
            return HRESULT_FROM_WIN32(WSAHOST_NOT_FOUND);
        }

        SOCKET sock = socket(result->ai_family, result->ai_socktype,
                             result->ai_protocol);
        if (sock == INVALID_SOCKET) {
            freeaddrinfo(result);
            return hresult_from_wsa();
        }

        /* Encoded pictures arrive from the MFT as complete access units. A
         * large I-frame can therefore enqueue hundreds of UDP datagrams in a
         * tight loop even though the average stream bitrate is modest. Give
         * Winsock room to absorb the burst. */
        int send_buffer_size = 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char *>(&send_buffer_size),
                   sizeof(send_buffer_size));

        auto *stream = new (std::nothrow)
            UdpByteStream(sock, bitrate_kbps);
        if (stream == nullptr) {
            closesocket(sock);
            freeaddrinfo(result);
            return E_OUTOFMEMORY;
        }
        memcpy(&stream->m_remote, result->ai_addr, result->ai_addrlen);
        stream->m_remote_len = (int)result->ai_addrlen;
        freeaddrinfo(result);
        *out = stream;
        return S_OK;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IMFByteStream) {
            *object = static_cast<IMFByteStream *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_refs);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG refs = (ULONG)InterlockedDecrement(&m_refs);
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    STDMETHODIMP GetCapabilities(DWORD *caps) override
    {
        if (caps == nullptr) {
            return E_POINTER;
        }
        *caps = MFBYTESTREAM_IS_WRITABLE;
        return S_OK;
    }
    STDMETHODIMP GetLength(QWORD *length) override
    {
        if (length == nullptr) {
            return E_POINTER;
        }
        *length = m_position;
        return S_OK;
    }
    STDMETHODIMP SetLength(QWORD) override { return E_NOTIMPL; }
    STDMETHODIMP GetCurrentPosition(QWORD *position) override
    {
        if (position == nullptr) {
            return E_POINTER;
        }
        *position = m_position;
        return S_OK;
    }
    STDMETHODIMP SetCurrentPosition(QWORD) override { return E_NOTIMPL; }
    STDMETHODIMP IsEndOfStream(BOOL *end) override
    {
        if (end == nullptr) {
            return E_POINTER;
        }
        *end = FALSE;
        return S_OK;
    }
    STDMETHODIMP Read(BYTE *, ULONG, ULONG *) override { return E_NOTIMPL; }
    STDMETHODIMP BeginRead(BYTE *, ULONG, IMFAsyncCallback *, IUnknown *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP EndRead(IMFAsyncResult *, ULONG *) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP Write(const BYTE *data, ULONG count, ULONG *written) override
    {
        if (written == nullptr || (data == nullptr && count != 0)) {
            return E_POINTER;
        }
        if (m_closed) {
            return MF_E_SHUTDOWN;
        }
        ULONG consumed = 0;
        while (consumed < count) {
            size_t copy_size = std::min<size_t>(
                count - consumed, m_pending.size() - m_pending_size);
            memcpy(m_pending.data() + m_pending_size, data + consumed,
                   copy_size);
            m_pending_size += copy_size;
            consumed += (ULONG)copy_size;
            if (m_pending_size == m_pending.size()) {
                HRESULT hr = send_packet(m_pending.data(), m_pending_size);
                if (FAILED(hr)) {
                    *written = consumed;
                    return hr;
                }
                m_pending_size = 0;
            }
        }
        *written = count;
        m_position += count;
        return S_OK;
    }

    STDMETHODIMP BeginWrite(const BYTE *data, ULONG count,
                            IMFAsyncCallback *callback, IUnknown *state) override
    {
        if (callback == nullptr) {
            return E_POINTER;
        }
        ULONG written = 0;
        HRESULT write_hr = Write(data, count, &written);
        m_last_async_write.store(written, std::memory_order_release);

        IMFAsyncResult *result = nullptr;
        HRESULT hr = MFCreateAsyncResult(nullptr, callback, state, &result);
        if (FAILED(hr)) {
            return hr;
        }
        result->SetStatus(write_hr);
        hr = MFInvokeCallback(result);
        result->Release();
        return hr;
    }
    STDMETHODIMP EndWrite(IMFAsyncResult *result, ULONG *written) override
    {
        if (result == nullptr || written == nullptr) {
            return E_POINTER;
        }
        *written = m_last_async_write.load(std::memory_order_acquire);
        return result->GetStatus();
    }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN, LONGLONG, DWORD,
                      QWORD *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP Flush() override { return send_remainder(); }
    STDMETHODIMP Close() override
    {
        if (!m_closed) {
            send_remainder();
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            m_closed = true;
        }
        return S_OK;
    }

private:
    explicit UdpByteStream(SOCKET sock, int bitrate_kbps) : m_socket(sock)
    {
        (void)bitrate_kbps;
    }
    ~UdpByteStream() { Close(); }

    /* Datagrams leave as soon as they are complete. Sleeping between them
     * costs a scheduler tick each time and holds every frame back; the large
     * send buffer absorbs keyframe bursts instead. */
    HRESULT send_packet(const uint8_t *data, size_t size)
    {
        int sent = sendto(m_socket, reinterpret_cast<const char *>(data),
                          (int)size, 0,
                          reinterpret_cast<const sockaddr *>(&m_remote),
                          m_remote_len);
        return sent == (int)size ? S_OK : hresult_from_wsa();
    }

    HRESULT send_remainder()
    {
        HRESULT hr = S_OK;
        if (m_pending_size != 0 && m_socket != INVALID_SOCKET) {
            hr = send_packet(m_pending.data(), m_pending_size);
            if (SUCCEEDED(hr)) {
                m_pending_size = 0;
            }
        }
        return hr;
    }

    LONG m_refs = 1;
    SOCKET m_socket = INVALID_SOCKET;
    sockaddr_storage m_remote{};
    int m_remote_len = 0;
    QWORD m_position = 0;
    bool m_closed = false;
    std::atomic<ULONG> m_last_async_write{0};
    std::array<uint8_t, STREAM_UDP_PAYLOAD> m_pending{};
    size_t m_pending_size = 0;
};

class MpegTsMux final {
public:
    explicit MpegTsMux(UdpByteStream *stream) : m_stream(stream) {}

    HRESULT Start()
    {
        m_last_tables_pts = 0;
        return WriteTables();
    }

    HRESULT WriteVideo(const uint8_t *data, size_t size, int64_t pts90,
                       bool keyframe)
    {
        if (data == nullptr || size == 0) {
            return S_OK;
        }
        if (keyframe || pts90 - m_last_tables_pts >= 9000) {
            HRESULT hr = WriteTables();
            if (FAILED(hr)) {
                return hr;
            }
            m_last_tables_pts = pts90;
        }
        HRESULT hr = WritePes(kVideoPid, 0xe0, data, size, pts90, true);
        if (SUCCEEDED(hr)) {
            m_last_pcr_pts = pts90;
            /* Send the frame's final partial datagram now, not with the
             * next write. */
            hr = m_stream->Flush();
        }
        return hr;
    }

    HRESULT WriteAudio(const uint8_t *data, size_t size, int64_t pts90)
    {
        if (data == nullptr || size == 0) {
            return S_OK;
        }

        /* Keep the program clock alive even when the configured video rate is
         * low.  An adaptation-only packet does not advance the video PID's
         * continuity counter because it carries no payload. */
        if (m_last_pcr_pts >= 0 && pts90 >= m_last_pcr_pts + 5400) {
            HRESULT hr = WritePcrOnly(pts90);
            if (FAILED(hr)) {
                return hr;
            }
            m_last_pcr_pts = pts90;
        }

        /* The inbox AAC MFT produces one raw AAC-LC block per sample. MPEG-TS
         * carries AAC as ADTS, so prepend the fixed 48-kHz stereo header. */
        size_t adts_size = size + 7;
        if (adts_size > 0x1fff) {
            return E_INVALIDARG;
        }
        std::vector<uint8_t> adts(adts_size);
        constexpr int profile = 1;      /* AAC Low Complexity */
        constexpr int sample_index = 3; /* 48000 Hz */
        constexpr int channels = STREAM_AUDIO_CHANNELS;
        adts[0] = 0xff;
        adts[1] = 0xf1;
        adts[2] = (uint8_t)((profile << 6) | (sample_index << 2) |
                            (channels >> 2));
        adts[3] = (uint8_t)(((channels & 3) << 6) |
                            ((adts_size >> 11) & 3));
        adts[4] = (uint8_t)(adts_size >> 3);
        adts[5] = (uint8_t)(((adts_size & 7) << 5) | 0x1f);
        adts[6] = 0xfc;
        memcpy(adts.data() + 7, data, size);
        HRESULT hr = WritePes(kAudioPid, 0xc0, adts.data(), adts.size(),
                              pts90, false);
        return SUCCEEDED(hr) ? m_stream->Flush() : hr;
    }

private:
    static constexpr uint16_t kPmtPid = 0x1000;
    static constexpr uint16_t kVideoPid = 0x0100;
    static constexpr uint16_t kAudioPid = 0x0101;

    static uint32_t SectionCrc(const uint8_t *data, size_t size)
    {
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < size; ++i) {
            crc ^= (uint32_t)data[i] << 24;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u
                                          : crc << 1;
            }
        }
        return crc;
    }

    static void AppendCrc(std::vector<uint8_t> &section)
    {
        uint32_t crc = SectionCrc(section.data(), section.size());
        section.push_back((uint8_t)(crc >> 24));
        section.push_back((uint8_t)(crc >> 16));
        section.push_back((uint8_t)(crc >> 8));
        section.push_back((uint8_t)crc);
    }

    uint8_t &Continuity(uint16_t pid)
    {
        if (pid == 0) return m_pat_cc;
        if (pid == kPmtPid) return m_pmt_cc;
        if (pid == kVideoPid) return m_video_cc;
        return m_audio_cc;
    }

    HRESULT SendPacket(const std::array<uint8_t, 188> &packet)
    {
        ULONG written = 0;
        return m_stream->Write(packet.data(), (ULONG)packet.size(), &written);
    }

    HRESULT WriteSection(uint16_t pid, const std::vector<uint8_t> &section)
    {
        if (section.size() > 183) {
            return E_INVALIDARG;
        }
        std::array<uint8_t, 188> packet;
        packet.fill(0xff);
        uint8_t &cc = Continuity(pid);
        packet[0] = 0x47;
        packet[1] = (uint8_t)(0x40 | ((pid >> 8) & 0x1f));
        packet[2] = (uint8_t)pid;
        packet[3] = (uint8_t)(0x10 | (cc++ & 0x0f));
        packet[4] = 0; /* pointer_field */
        memcpy(packet.data() + 5, section.data(), section.size());
        return SendPacket(packet);
    }

    HRESULT WriteTables()
    {
        std::vector<uint8_t> pat = {
            0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00,
            0x00, 0x01, (uint8_t)(0xe0 | (kPmtPid >> 8)),
            (uint8_t)kPmtPid,
        };
        AppendCrc(pat);
        HRESULT hr = WriteSection(0, pat);
        if (FAILED(hr)) {
            return hr;
        }

        std::vector<uint8_t> pmt = {
            0x02, 0xb0, 0x17, 0x00, 0x01, 0xc1, 0x00, 0x00,
            (uint8_t)(0xe0 | (kVideoPid >> 8)), (uint8_t)kVideoPid,
            0xf0, 0x00,
            0x1b, (uint8_t)(0xe0 | (kVideoPid >> 8)), (uint8_t)kVideoPid,
            0xf0, 0x00,
            0x0f, (uint8_t)(0xe0 | (kAudioPid >> 8)), (uint8_t)kAudioPid,
            0xf0, 0x00,
        };
        AppendCrc(pmt);
        return WriteSection(kPmtPid, pmt);
    }

    static void WritePts(uint8_t *out, int64_t pts)
    {
        uint64_t value = (uint64_t)pts & ((1ull << 33) - 1);
        out[0] = (uint8_t)(0x20 | (((value >> 30) & 7) << 1) | 1);
        out[1] = (uint8_t)(value >> 22);
        out[2] = (uint8_t)((((value >> 15) & 0x7f) << 1) | 1);
        out[3] = (uint8_t)(value >> 7);
        out[4] = (uint8_t)(((value & 0x7f) << 1) | 1);
    }

    static void WritePcr(uint8_t *out, int64_t pts90)
    {
        uint64_t base = (uint64_t)pts90 & ((1ull << 33) - 1);
        out[0] = (uint8_t)(base >> 25);
        out[1] = (uint8_t)(base >> 17);
        out[2] = (uint8_t)(base >> 9);
        out[3] = (uint8_t)(base >> 1);
        out[4] = (uint8_t)(((base & 1) << 7) | 0x7e);
        out[5] = 0;
    }

    HRESULT WritePcrOnly(int64_t pts90)
    {
        std::array<uint8_t, 188> packet;
        packet.fill(0xff);
        packet[0] = 0x47;
        packet[1] = (uint8_t)((kVideoPid >> 8) & 0x1f);
        packet[2] = (uint8_t)kVideoPid;
        packet[3] = (uint8_t)(0x20 | ((m_video_cc - 1) & 0x0f));
        packet[4] = 183;
        packet[5] = 0x10;
        WritePcr(packet.data() + 6, pts90);
        return SendPacket(packet);
    }

    HRESULT WritePes(uint16_t pid, uint8_t stream_id, const uint8_t *data,
                     size_t size, int64_t pts90, bool with_pcr)
    {
        std::vector<uint8_t> pes(14 + size);
        pes[0] = 0x00;
        pes[1] = 0x00;
        pes[2] = 0x01;
        pes[3] = stream_id;
        size_t pes_length = size + 8;
        if (stream_id >= 0xe0 || pes_length > 0xffff) {
            pes[4] = 0;
            pes[5] = 0;
        } else {
            pes[4] = (uint8_t)(pes_length >> 8);
            pes[5] = (uint8_t)pes_length;
        }
        pes[6] = 0x80;
        pes[7] = 0x80;
        pes[8] = 5;
        WritePts(pes.data() + 9, pts90);
        memcpy(pes.data() + 14, data, size);

        size_t offset = 0;
        bool first = true;
        uint8_t &cc = Continuity(pid);
        while (offset < pes.size()) {
            bool pcr = first && with_pcr;
            size_t max_payload = pcr ? 176 : 184;
            size_t payload = std::min(max_payload, pes.size() - offset);
            bool adaptation = pcr || payload < 184;
            std::array<uint8_t, 188> packet;
            packet.fill(0xff);
            packet[0] = 0x47;
            packet[1] = (uint8_t)(((pid >> 8) & 0x1f) |
                                  (first ? 0x40 : 0));
            packet[2] = (uint8_t)pid;
            packet[3] = (uint8_t)((adaptation ? 0x30 : 0x10) |
                                  (cc++ & 0x0f));

            size_t payload_offset = 4;
            if (adaptation) {
                size_t total = 184 - payload;
                packet[4] = (uint8_t)(total - 1);
                payload_offset = 4 + total;
                if (total > 1) {
                    packet[5] = pcr ? 0x10 : 0x00;
                    if (pcr) {
                        WritePcr(packet.data() + 6, pts90);
                    }
                }
            }
            memcpy(packet.data() + payload_offset, pes.data() + offset,
                   payload);
            HRESULT hr = SendPacket(packet);
            if (FAILED(hr)) {
                return hr;
            }
            offset += payload;
            first = false;
        }
        return S_OK;
    }

    UdpByteStream *m_stream;
    uint8_t m_pat_cc = 0;
    uint8_t m_pmt_cc = 0;
    uint8_t m_video_cc = 0;
    uint8_t m_audio_cc = 0;
    int64_t m_last_tables_pts = 0;
    int64_t m_last_pcr_pts = -1;
};

/* ProcessInput normally retains its sample until the transform has consumed
 * it. Reusing a buffer as soon as ProcessInput returns can therefore overwrite
 * a frame that the hardware encoder is still reading. IMFTrackedSample calls
 * this pool back only after every encoder-held reference is gone. */
class MfVideoInputPool final : public IMFAsyncCallback {
public:
    static HRESULT Create(DWORD buffer_size, size_t count,
                          MfVideoInputPool **out)
    {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;

        auto *pool = new (std::nothrow) MfVideoInputPool();
        if (pool == nullptr) {
            return E_OUTOFMEMORY;
        }

        HRESULT hr = S_OK;
        try {
            pool->m_available.reserve(count);
        } catch (...) {
            hr = E_OUTOFMEMORY;
        }
        for (size_t i = 0; i < count && SUCCEEDED(hr); ++i) {
            IMFTrackedSample *tracked = nullptr;
            IMFSample *sample = nullptr;
            IMFMediaBuffer *buffer = nullptr;

            hr = MFCreateTrackedSample(&tracked);
            if (SUCCEEDED(hr)) {
                hr = tracked->QueryInterface(IID_PPV_ARGS(&sample));
            }
            if (SUCCEEDED(hr)) {
                hr = MFCreateMemoryBuffer(buffer_size, &buffer);
            }
            if (SUCCEEDED(hr)) {
                hr = sample->AddBuffer(buffer);
            }
            if (SUCCEEDED(hr)) {
                try {
                    pool->m_available.push_back(sample);
                    sample = nullptr;
                } catch (...) {
                    hr = E_OUTOFMEMORY;
                }
            }

            if (buffer != nullptr) {
                buffer->Release();
            }
            if (sample != nullptr) {
                sample->Release();
            }
            if (tracked != nullptr) {
                tracked->Release();
            }
        }
        if (FAILED(hr)) {
            pool->Release();
            return hr;
        }

        *out = pool;
        return S_OK;
    }

    HRESULT Acquire(IMFSample **sample, IMFMediaBuffer **buffer)
    {
        if (sample == nullptr || buffer == nullptr) {
            return E_POINTER;
        }
        *sample = nullptr;
        *buffer = nullptr;

        IMFSample *available = nullptr;
        g_mutex_lock(&m_lock);
        if (!m_shutdown && !m_available.empty()) {
            available = m_available.back();
            m_available.pop_back();
        }
        g_mutex_unlock(&m_lock);
        if (available == nullptr) {
            return MF_E_SAMPLEALLOCATOR_EMPTY;
        }

        HRESULT hr = available->DeleteAllItems();
        if (SUCCEEDED(hr)) {
            hr = available->SetSampleFlags(0);
        }
        IMFMediaBuffer *available_buffer = nullptr;
        if (SUCCEEDED(hr)) {
            hr = available->GetBufferByIndex(0, &available_buffer);
        }
        if (SUCCEEDED(hr)) {
            hr = available_buffer->SetCurrentLength(0);
        }

        IMFTrackedSample *tracked = nullptr;
        if (SUCCEEDED(hr)) {
            hr = available->QueryInterface(
                XEMU_IID_IMFTrackedSample,
                reinterpret_cast<void **>(&tracked));
        }
        if (SUCCEEDED(hr)) {
            hr = tracked->SetAllocator(this, nullptr);
        }
        if (SUCCEEDED(hr)) {
            g_mutex_lock(&m_lock);
            ++m_outstanding;
            g_mutex_unlock(&m_lock);
        }
        if (tracked != nullptr) {
            tracked->Release();
        }
        if (FAILED(hr)) {
            if (available_buffer != nullptr) {
                available_buffer->Release();
            }
            ReturnAvailable(available);
            return hr;
        }

        *sample = available;
        *buffer = available_buffer;
        return S_OK;
    }

    void Shutdown()
    {
        std::vector<IMFSample *> available;
        g_mutex_lock(&m_lock);
        m_shutdown = true;
        available.swap(m_available);
        g_mutex_unlock(&m_lock);
        for (IMFSample *sample : available) {
            sample->Release();
        }
    }

    void WaitForCallbacks()
    {
        g_mutex_lock(&m_lock);
        while (m_outstanding != 0) {
            g_cond_wait(&m_cond, &m_lock);
        }
        g_mutex_unlock(&m_lock);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IMFAsyncCallback) {
            *object = static_cast<IMFAsyncCallback *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_refs);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG refs = (ULONG)InterlockedDecrement(&m_refs);
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    STDMETHODIMP GetParameters(DWORD *, DWORD *) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP Invoke(IMFAsyncResult *result) override
    {
        if (result == nullptr) {
            return E_POINTER;
        }

        IUnknown *object = nullptr;
        HRESULT hr = result->GetObject(&object);
        IMFSample *sample = nullptr;
        if (SUCCEEDED(hr)) {
            hr = object->QueryInterface(IID_PPV_ARGS(&sample));
        }
        if (object != nullptr) {
            object->Release();
        }
        if (SUCCEEDED(hr)) {
            ReturnAvailable(sample);
        }
        g_mutex_lock(&m_lock);
        if (m_outstanding != 0) {
            --m_outstanding;
        }
        g_cond_broadcast(&m_cond);
        g_mutex_unlock(&m_lock);
        return hr;
    }

private:
    MfVideoInputPool()
    {
        g_mutex_init(&m_lock);
        g_cond_init(&m_cond);
    }
    ~MfVideoInputPool()
    {
        Shutdown();
        g_cond_clear(&m_cond);
        g_mutex_clear(&m_lock);
    }

    void ReturnAvailable(IMFSample *sample)
    {
        bool release = false;
        g_mutex_lock(&m_lock);
        if (m_shutdown) {
            release = true;
        } else {
            try {
                m_available.push_back(sample);
            } catch (...) {
                release = true;
            }
        }
        g_mutex_unlock(&m_lock);
        if (release) {
            sample->Release();
        }
    }

    LONG m_refs = 1;
    GMutex m_lock;
    GCond m_cond;
    bool m_shutdown = false;
    size_t m_outstanding = 0;
    std::vector<IMFSample *> m_available;
};

struct StreamEncoder {
    StreamParams params{};
    IMFTransform *encoder = nullptr;
    IMFTransform *audio_encoder = nullptr;
    UdpByteStream *stream = nullptr;
    MpegTsMux *mux = nullptr;
    int out_w = 0;
    int out_h = 0;
    LONGLONG pts = 0;
    int64_t timeline_start_us = 0;
    LONGLONG last_video_time = -1;
    int64_t last_video_slot = -1;
    bool audio_anchored = false;
    int64_t audio_pts_samples = 0;
    int64_t audio_output_samples = 0;
    GByteArray *audio_pcm = nullptr;
    std::vector<int> scale_x;
    std::vector<int> scale_y;
    int scale_source_w = 0;
    int scale_source_h = 0;
    MfVideoInputPool *video_input_pool = nullptr;
    bool video_input_pool_active = false;
    MFT_OUTPUT_STREAM_INFO video_output_info{};
    bool video_output_info_valid = false;
    IMFSample *video_output_sample = nullptr;
    IMFMediaBuffer *video_output_buffer = nullptr;
    bool mf_started = false;
};

static void encoder_prepare_video_sample_pools(StreamEncoder *e)
{
    DWORD input_size = (DWORD)((size_t)e->out_w * (size_t)e->out_h * 3 / 2);
    HRESULT pool_hr = MfVideoInputPool::Create(
        input_size, STREAM_VIDEO_INPUT_POOL_MAX, &e->video_input_pool);
    if (FAILED(pool_hr)) {
        /* Fall back to allocating a buffer per frame when the system MFT does
         * not support tracked samples. */
        fprintf(stderr,
                "xemu stream: video input pool unavailable (0x%08lx); "
                "using per-frame buffers\n",
                (long)pool_hr);
    } else {
        e->video_input_pool_active = true;
    }

    HRESULT output_hr =
        e->encoder->GetOutputStreamInfo(0, &e->video_output_info);
    if (SUCCEEDED(output_hr)) {
        e->video_output_info_valid = true;
    }
    if (SUCCEEDED(output_hr) &&
        !(e->video_output_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        DWORD capacity =
            std::max<DWORD>(e->video_output_info.cbSize, 2 * 1024 * 1024);
        output_hr = MFCreateSample(&e->video_output_sample);
        if (SUCCEEDED(output_hr)) {
            output_hr = MFCreateMemoryBuffer(capacity, &e->video_output_buffer);
        }
        if (SUCCEEDED(output_hr)) {
            output_hr =
                e->video_output_sample->AddBuffer(e->video_output_buffer);
        }
    }
    if (FAILED(output_hr)) {
        if (e->video_output_buffer != nullptr) {
            e->video_output_buffer->Release();
            e->video_output_buffer = nullptr;
        }
        if (e->video_output_sample != nullptr) {
            e->video_output_sample->Release();
            e->video_output_sample = nullptr;
        }
        fprintf(stderr,
                "xemu stream: reusable video output sample unavailable "
                "(0x%08lx); using per-output buffers\n",
                (long)output_hr);
    }
}

static HRESULT set_video_type_common(IMFMediaType *type, const GUID &subtype,
                                     int width, int height, int fps)
{
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE,
                                               width, height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE,
                                                fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO,
                                                1, 1);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE,
                                            MFVideoInterlace_Progressive);
    return hr;
}

static HRESULT set_audio_type_common(IMFMediaType *type, const GUID &subtype)
{
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                             STREAM_AUDIO_RATE);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                             STREAM_AUDIO_CHANNELS);
    }
    return hr;
}

static HRESULT enumerate_encoder(const GUID &category, const GUID &major_type,
                                 const GUID &subtype, UINT32 flags,
                                 const char *name, IMFTransform **encoder,
                                 UINT32 *candidate_count)
{
    MFT_REGISTER_TYPE_INFO output_info = { major_type, subtype };
    IMFActivate **activations = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(category, flags, nullptr, &output_info,
                           &activations, &count);
    if (candidate_count != nullptr) {
        *candidate_count = count;
    }
    fprintf(stderr,
            "xemu stream: %s MFTEnumEx flags=0x%08x hr=0x%08lx "
            "candidates=%u\n",
            name, flags, (long)hr, count);
    fflush(stderr);

    HRESULT activate_hr = MF_E_TOPO_CODEC_NOT_FOUND;
    if (SUCCEEDED(hr)) {
        for (UINT32 i = 0; i < count; ++i) {
            activate_hr = activations[i]->ActivateObject(
                IID_PPV_ARGS(encoder));
            fprintf(stderr,
                    "xemu stream: %s candidate %u activation hr=0x%08lx\n",
                    name, i, (long)activate_hr);
            fflush(stderr);
            if (SUCCEEDED(activate_hr)) {
                break;
            }
        }
    }
    for (UINT32 i = 0; i < count; ++i) {
        activations[i]->Release();
    }
    CoTaskMemFree(activations);
    if (FAILED(hr)) {
        return hr;
    }
    return activate_hr;
}

/* Live-streaming encoder settings: constant bitrate, no B-frames, no frame
 * lookahead, and a keyframe every second so a receiver that joins late or
 * loses a packet recovers within a second. Encoders that reject a property
 * keep their default for it. */
static void configure_live_encoder(IMFTransform *encoder, int fps,
                                   int bitrate_kbps)
{
    static const IID kCodecApi = {
        0x901db4c7, 0x31ce, 0x41a2,
        { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda }
    };
    static const GUID kRateControl = { STATIC_CODECAPI_AVEncCommonRateControlMode };
    static const GUID kMeanBitRate = { STATIC_CODECAPI_AVEncCommonMeanBitRate };
    static const GUID kScenario = { STATIC_CODECAPI_AVScenarioInfo };
    static const GUID kGopSize = { STATIC_CODECAPI_AVEncMPVGOPSize };
    static const GUID kBFrames = { STATIC_CODECAPI_AVEncMPVDefaultBPictureCount };
    static const GUID kLowLatency = { STATIC_CODECAPI_AVLowLatencyMode };

    XemuCodecApi *api = nullptr;
    if (FAILED(encoder->QueryInterface(kCodecApi,
                                       reinterpret_cast<void **>(&api)))) {
        fprintf(stderr, "xemu stream: encoder has no codec settings\n");
        return;
    }
    auto set_value = [api](const GUID &property, VARTYPE type, ULONG value,
                           const char *name) {
        VARIANT variant;
        memset(&variant, 0, sizeof(variant));
        variant.vt = type;
        if (type == VT_BOOL) {
            variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        } else {
            variant.ulVal = value;
        }
        HRESULT hr = api->SetValue(&property, &variant);
        if (FAILED(hr)) {
            fprintf(stderr, "xemu stream: encoder %s not set (0x%08lx)\n",
                    name, (long)hr);
        }
    };
    set_value(kRateControl, VT_UI4, eAVEncCommonRateControlMode_CBR,
              "rate control");
    set_value(kMeanBitRate, VT_UI4, (ULONG)std::max(100, bitrate_kbps) * 1000,
              "bitrate");
    set_value(kScenario, VT_UI4, eAVScenarioInfo_LiveStreaming, "scenario");
    set_value(kGopSize, VT_UI4, (ULONG)std::max(1, fps), "keyframe interval");
    set_value(kBFrames, VT_UI4, 0, "B-frames");
    set_value(kLowLatency, VT_BOOL, 1, "low latency");
    api->Release();
}

static HRESULT create_encoder(const CLSID &preferred_clsid,
                              const GUID &category, const GUID &major_type,
                              const GUID &subtype, const char *name,
                              IMFTransform **encoder)
{
    *encoder = nullptr;
    HRESULT direct_hr = CoCreateInstance(preferred_clsid, nullptr,
                                         CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(encoder));
    fprintf(stderr, "xemu stream: %s direct activation hr=0x%08lx\n",
            name, (long)direct_hr);
    fflush(stderr);
    if (SUCCEEDED(direct_hr)) {
        return direct_hr;
    }

    constexpr UINT32 sync_flags =
        XEMU_MFT_ENUM_FLAG_SYNCMFT | XEMU_MFT_ENUM_FLAG_LOCALMFT |
        XEMU_MFT_ENUM_FLAG_SORTANDFILTER;
    UINT32 count = 0;
    HRESULT hr = enumerate_encoder(category, major_type, subtype, sync_flags,
                                   name, encoder, &count);
    if (SUCCEEDED(hr)) {
        return hr;
    }

    /* Some Xbox system codecs are exposed as asynchronous or hardware MFTs
     * even when the corresponding desktop COM class is not registered inside
     * the AppContainer. Include those categories as a second discovery pass. */
    constexpr UINT32 broad_flags =
        XEMU_MFT_ENUM_FLAG_SYNCMFT | XEMU_MFT_ENUM_FLAG_ASYNCMFT |
        XEMU_MFT_ENUM_FLAG_HARDWARE | XEMU_MFT_ENUM_FLAG_LOCALMFT |
        XEMU_MFT_ENUM_FLAG_TRANSCODE_ONLY |
        XEMU_MFT_ENUM_FLAG_SORTANDFILTER;
    hr = enumerate_encoder(category, major_type, subtype, broad_flags, name,
                           encoder, &count);
    return SUCCEEDED(hr) ? hr : (count == 0 ? direct_hr : hr);
}

static HRESULT select_aac_output_type(IMFTransform *encoder,
                                      IMFMediaType **selected_type,
                                      UINT32 *available_count)
{
    constexpr UINT32 target_bytes_per_second = 16000; /* 128 kbit/s */
    IMFMediaType *best = nullptr;
    uint64_t best_delta = UINT64_MAX;
    UINT32 count = 0;
    HRESULT enumeration_hr = S_OK;

    for (DWORD index = 0;; ++index) {
        IMFMediaType *candidate = nullptr;
        HRESULT hr = encoder->GetOutputAvailableType(0, index, &candidate);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr)) {
            enumeration_hr = hr;
            break;
        }
        ++count;

        GUID major = GUID_NULL;
        GUID subtype = GUID_NULL;
        UINT32 rate = 0;
        UINT32 channels = 0;
        UINT32 bits = 0;
        UINT32 bytes_per_second = 0;
        UINT32 bitrate = 0;
        candidate->GetGUID(MF_MT_MAJOR_TYPE, &major);
        candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
        candidate->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        candidate->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        if (FAILED(candidate->GetUINT32(
                MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes_per_second)) &&
            SUCCEEDED(candidate->GetUINT32(MF_MT_AVG_BITRATE, &bitrate))) {
            bytes_per_second = bitrate / 8;
        }
        if (IsEqualGUID(major, MFMediaType_Audio) &&
            IsEqualGUID(subtype, MFAudioFormat_AAC) &&
            rate == STREAM_AUDIO_RATE &&
            channels == STREAM_AUDIO_CHANNELS && bits == 16) {
            uint64_t delta = bytes_per_second > target_bytes_per_second
                                 ? bytes_per_second - target_bytes_per_second
                                 : target_bytes_per_second - bytes_per_second;
            if (best == nullptr || delta < best_delta) {
                if (best != nullptr) {
                    best->Release();
                }
                best = candidate;
                candidate = nullptr;
                best_delta = delta;
            }
        }
        if (candidate != nullptr) {
            candidate->Release();
        }
    }
    if (available_count != nullptr) {
        *available_count = count;
    }
    if (best == nullptr) {
        return FAILED(enumeration_hr) ? enumeration_hr
                                      : MF_E_INVALIDMEDIATYPE;
    }

    /* MPEG-TS needs ADTS framing. Request raw AAC blocks from the MFT and add
     * the ADTS header in MpegTsMux::WriteAudio so the payload is deterministic
     * across the inbox encoder implementations. */
    HRESULT hr = best->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    if (SUCCEEDED(hr)) {
        hr = encoder->SetOutputType(0, best, 0);
    }
    if (SUCCEEDED(hr)) {
        *selected_type = best;
    } else {
        best->Release();
    }
    return hr;
}

static bool encoder_open(StreamEncoder *e, int src_w, int src_h,
                         int64_t timeline_start_us)
{
    HRESULT hr;
    const char *step = "MFStartup";
    IMFMediaType *output = nullptr;
    IMFMediaType *input = nullptr;
    IMFMediaType *audio_output = nullptr;
    IMFMediaType *audio_input = nullptr;

    e->timeline_start_us = timeline_start_us;
    compute_output_size(src_w, src_h, &e->out_w, &e->out_h);
    /* Sink writers and their container media sinks require the full platform;
     * MFSTARTUP_LITE is only sufficient for transforms. */
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        set_status("stream error: MFStartup failed (0x%08lx)", (long)hr);
        return false;
    }
    e->mf_started = true;

    step = "UDP stream creation";
    hr = UdpByteStream::Create(e->params.url, e->params.bitrate_kbps,
                               &e->stream);
    if (SUCCEEDED(hr)) {
        step = "H.264 encoder activation";
        hr = create_encoder(CLSID_CMSH264EncoderMFT,
                            MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video,
                            MFVideoFormat_H264, "H.264", &e->encoder);
    }
    if (SUCCEEDED(hr)) {
        configure_live_encoder(e->encoder, e->params.fps,
                               e->params.bitrate_kbps);
    }
    if (SUCCEEDED(hr)) step = "H.264 output type creation";
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&output);
    if (SUCCEEDED(hr)) {
        step = "H.264 output type attributes";
        hr = set_video_type_common(output, MFVideoFormat_H264, e->out_w,
                                   e->out_h, e->params.fps);
    }
    if (SUCCEEDED(hr)) {
        hr = output->SetUINT32(MF_MT_AVG_BITRATE,
                               (UINT32)e->params.bitrate_kbps * 1000);
    }
    if (SUCCEEDED(hr)) {
        hr = output->SetUINT32(MF_MT_MPEG2_PROFILE, 77); /* H.264 Main */
    }
    if (SUCCEEDED(hr)) {
        step = "H.264 SetOutputType";
        /* Let the encoder select a compatible level.  Hard-coding level 3.1
         * rejects otherwise valid 720p60 configurations on conforming MFTs. */
        hr = e->encoder->SetOutputType(0, output, 0);
    }
    if (SUCCEEDED(hr)) step = "H.264 input type creation";
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&input);
    if (SUCCEEDED(hr)) {
        step = "H.264 input type attributes";
        hr = set_video_type_common(input, MFVideoFormat_NV12, e->out_w,
                                   e->out_h, e->params.fps);
    }
    if (SUCCEEDED(hr)) {
        step = "H.264 SetInputType";
        hr = e->encoder->SetInputType(0, input, 0);
    }
    if (SUCCEEDED(hr)) {
        encoder_prepare_video_sample_pools(e);
    }
    if (SUCCEEDED(hr)) {
        step = "H.264 begin streaming";
        hr = e->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    }
    if (SUCCEEDED(hr)) {
        step = "H.264 start stream";
        hr = e->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (SUCCEEDED(hr)) {
        step = "AAC encoder activation";
        hr = create_encoder(CLSID_AACMFTEncoder, MFT_CATEGORY_AUDIO_ENCODER,
                            MFMediaType_Audio, MFAudioFormat_AAC, "AAC",
                            &e->audio_encoder);
    }
    if (SUCCEEDED(hr)) step = "AAC input type creation";
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&audio_input);
    if (SUCCEEDED(hr)) {
        step = "AAC input type attributes";
        hr = set_audio_type_common(audio_input, MFAudioFormat_PCM);
    }
    if (SUCCEEDED(hr)) {
        hr = audio_input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                                    STREAM_AUDIO_BYTES_PER_FRAME);
    }
    if (SUCCEEDED(hr)) {
        hr = audio_input->SetUINT32(
            MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
            STREAM_AUDIO_RATE * STREAM_AUDIO_BYTES_PER_FRAME);
    }
    if (SUCCEEDED(hr)) {
        hr = audio_input->SetUINT32(
            MF_MT_AVG_BITRATE,
            STREAM_AUDIO_RATE * STREAM_AUDIO_BYTES_PER_FRAME * 8);
    }
    if (SUCCEEDED(hr)) {
        step = "AAC SetInputType";
        hr = e->audio_encoder->SetInputType(0, audio_input, 0);
    }

    UINT32 available_aac_types = 0;
    if (SUCCEEDED(hr)) {
        step = "AAC output type enumeration";
        hr = select_aac_output_type(e->audio_encoder, &audio_output,
                                    &available_aac_types);
        fprintf(stderr,
                "xemu stream: AAC output media types=%u selection "
                "hr=0x%08lx\n",
                available_aac_types, (long)hr);
        fflush(stderr);
    }
    if (SUCCEEDED(hr)) {
        step = "AAC begin streaming";
        hr = e->audio_encoder->ProcessMessage(
            MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    }
    if (SUCCEEDED(hr)) {
        step = "AAC start stream";
        hr = e->audio_encoder->ProcessMessage(
            MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (SUCCEEDED(hr)) {
        step = "MPEG-TS mux allocation";
        e->mux = new (std::nothrow) MpegTsMux(e->stream);
        if (e->mux == nullptr) {
            hr = E_OUTOFMEMORY;
        }
    }
    if (SUCCEEDED(hr)) {
        step = "MPEG-TS table write";
        hr = e->mux->Start();
    }
    if (SUCCEEDED(hr)) {
        step = "audio queue allocation";
        e->audio_pcm = g_byte_array_new();
        if (e->audio_pcm == nullptr) {
            hr = E_OUTOFMEMORY;
        }
    }

    if (input != nullptr) input->Release();
    if (output != nullptr) output->Release();
    if (audio_input != nullptr) audio_input->Release();
    if (audio_output != nullptr) audio_output->Release();

    if (FAILED(hr)) {
        set_status("stream error: %s failed (0x%08lx)", step, (long)hr);
        return false;
    }
    g_mutex_lock(&s_lock);
    s_output_width = e->out_w;
    s_output_height = e->out_h;
    g_mutex_unlock(&s_lock);
    s_ready.store(true, std::memory_order_release);
    set_status("streaming %s (%dx%d @ %d fps, MPEG-TS H.264 + AAC)",
               e->params.url, e->out_w, e->out_h, e->params.fps);
    return true;
}

static inline uint8_t clamp_byte(int value)
{
    return (uint8_t)std::max(0, std::min(255, value));
}

static void bgra_to_nv12_scaled(StreamEncoder *e, const StreamFrame *f,
                                uint8_t *dst)
{
    int out_w = e->out_w;
    int out_h = e->out_h;
    uint8_t *y_plane = dst;
    uint8_t *uv_plane = dst + (size_t)out_w * out_h;

    if (e->scale_source_w != f->width ||
        e->scale_source_h != f->height ||
        e->scale_x.size() != (size_t)out_w ||
        e->scale_y.size() != (size_t)out_h) {
        e->scale_x.resize(out_w);
        e->scale_y.resize(out_h);
        for (int x = 0; x < out_w; ++x) {
            e->scale_x[x] =
                (int)((uint64_t)x * (uint64_t)f->width / (uint64_t)out_w);
        }
        for (int y = 0; y < out_h; ++y) {
            e->scale_y[y] =
                (int)((uint64_t)y * (uint64_t)f->height / (uint64_t)out_h);
        }
        e->scale_source_w = f->width;
        e->scale_source_h = f->height;
    }

    for (int y = 0; y < out_h; ++y) {
        const uint8_t *row = f->data + (size_t)e->scale_y[y] * f->pitch;
        for (int x = 0; x < out_w; ++x) {
            const uint8_t *p = row + (size_t)e->scale_x[x] * 4;
            int b = p[0], g = p[1], r = p[2];
            y_plane[(size_t)y * out_w + x] =
                clamp_byte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int y = 0; y < out_h; y += 2) {
        const uint8_t *row = f->data + (size_t)e->scale_y[y] * f->pitch;
        for (int x = 0; x < out_w; x += 2) {
            const uint8_t *p = row + (size_t)e->scale_x[x] * 4;
            int b = p[0], g = p[1], r = p[2];
            size_t uv = (size_t)(y / 2) * out_w + x;
            uv_plane[uv] =
                clamp_byte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uv_plane[uv + 1] =
                clamp_byte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

static HRESULT encoder_drain_output(StreamEncoder *e, bool *produced)
{
    *produced = false;
    if (!e->video_output_info_valid) {
        HRESULT info_hr =
            e->encoder->GetOutputStreamInfo(0, &e->video_output_info);
        if (FAILED(info_hr)) {
            return info_hr;
        }
        e->video_output_info_valid = true;
    }

    IMFSample *caller_sample = e->video_output_sample;
    IMFMediaBuffer *caller_buffer = e->video_output_buffer;
    bool release_caller_sample = false;
    bool release_caller_buffer = false;
    HRESULT hr = S_OK;
    if (!(e->video_output_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) &&
        caller_sample == nullptr) {
        DWORD capacity =
            std::max<DWORD>(e->video_output_info.cbSize, 2 * 1024 * 1024);
        hr = MFCreateSample(&caller_sample);
        if (SUCCEEDED(hr)) {
            release_caller_sample = true;
            hr = MFCreateMemoryBuffer(capacity, &caller_buffer);
        }
        if (SUCCEEDED(hr)) {
            release_caller_buffer = true;
            hr = caller_sample->AddBuffer(caller_buffer);
        }
        if (FAILED(hr)) {
            if (release_caller_buffer && caller_buffer != nullptr) {
                caller_buffer->Release();
            }
            if (release_caller_sample && caller_sample != nullptr) {
                caller_sample->Release();
            }
            return hr;
        }
    }
    if (caller_sample != nullptr) {
        hr = caller_sample->DeleteAllItems();
        if (SUCCEEDED(hr)) {
            hr = caller_sample->SetSampleFlags(0);
        }
        if (SUCCEEDED(hr) && caller_buffer != nullptr) {
            hr = caller_buffer->SetCurrentLength(0);
        }
        if (FAILED(hr)) {
            if (release_caller_buffer && caller_buffer != nullptr) {
                caller_buffer->Release();
            }
            if (release_caller_sample) {
                caller_sample->Release();
            }
            return hr;
        }
    }

    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;
    output.pSample = caller_sample;
    DWORD status = 0;
    hr = e->encoder->ProcessOutput(0, 1, &output, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        hr = S_OK;
        *produced = false;
    } else if (SUCCEEDED(hr) && output.pSample != nullptr) {
        IMFMediaBuffer *encoded = nullptr;
        BYTE *data = nullptr;
        DWORD length = 0;
        LONGLONG sample_time = 0;
        UINT32 clean_point = 0;
        if (FAILED(output.pSample->GetSampleTime(&sample_time))) {
            sample_time = std::max<LONGLONG>(0, e->last_video_time);
        }
        sample_time = std::max<LONGLONG>(0, sample_time);
        output.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point);
        hr = output.pSample->ConvertToContiguousBuffer(&encoded);
        if (SUCCEEDED(hr))
            hr = encoded->Lock(&data, nullptr, &length);
        if (SUCCEEDED(hr) && length != 0 && e->mux != nullptr) {
            int64_t pts90 = sample_time * 9 / 1000;
            hr = e->mux->WriteVideo(data, length, pts90, clean_point != 0);
        }
        if (data != nullptr)
            encoded->Unlock();
        if (encoded != nullptr)
            encoded->Release();
        *produced = SUCCEEDED(hr);
    }
    if (output.pEvents != nullptr)
        output.pEvents->Release();
    if (output.pSample != nullptr && output.pSample != caller_sample) {
        output.pSample->Release();
    }
    if (release_caller_buffer && caller_buffer != nullptr) {
        caller_buffer->Release();
    }
    if (release_caller_sample && caller_sample != nullptr) {
        caller_sample->Release();
    }
    return hr;
}

static HRESULT encoder_drain_audio_output(StreamEncoder *e, bool *produced)
{
    *produced = false;
    MFT_OUTPUT_STREAM_INFO info{};
    HRESULT hr = e->audio_encoder->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) {
        return hr;
    }

    IMFSample *allocated_sample = nullptr;
    IMFMediaBuffer *allocated_buffer = nullptr;
    if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        DWORD capacity = std::max<DWORD>(info.cbSize, 64 * 1024);
        hr = MFCreateSample(&allocated_sample);
        if (SUCCEEDED(hr)) {
            hr = MFCreateMemoryBuffer(capacity, &allocated_buffer);
        }
        if (SUCCEEDED(hr)) {
            hr = allocated_sample->AddBuffer(allocated_buffer);
        }
        if (FAILED(hr)) {
            if (allocated_buffer != nullptr) allocated_buffer->Release();
            if (allocated_sample != nullptr) allocated_sample->Release();
            return hr;
        }
    }

    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;
    output.pSample = allocated_sample;
    DWORD status = 0;
    hr = e->audio_encoder->ProcessOutput(0, 1, &output, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        hr = S_OK;
    } else if (SUCCEEDED(hr) && output.pSample != nullptr) {
        IMFMediaBuffer *encoded = nullptr;
        BYTE *data = nullptr;
        DWORD length = 0;
        LONGLONG sample_time = 0;
        if (FAILED(output.pSample->GetSampleTime(&sample_time))) {
            sample_time = e->audio_output_samples * 10000000LL /
                          STREAM_AUDIO_RATE;
        }
        sample_time = std::max<LONGLONG>(0, sample_time);
        hr = output.pSample->ConvertToContiguousBuffer(&encoded);
        if (SUCCEEDED(hr)) hr = encoded->Lock(&data, nullptr, &length);
        if (SUCCEEDED(hr) && length != 0 && e->mux != nullptr) {
            hr = e->mux->WriteAudio(data, length, sample_time * 9 / 1000);
        }
        if (data != nullptr) encoded->Unlock();
        if (encoded != nullptr) encoded->Release();
        if (SUCCEEDED(hr)) {
            e->audio_output_samples += STREAM_AUDIO_SAMPLES;
            *produced = true;
        }
    }
    if (output.pEvents != nullptr) output.pEvents->Release();
    if (output.pSample != nullptr && output.pSample != allocated_sample) {
        output.pSample->Release();
    }
    if (allocated_buffer != nullptr) allocated_buffer->Release();
    if (allocated_sample != nullptr) allocated_sample->Release();
    return hr;
}

static bool encoder_write_audio_block(StreamEncoder *e, const uint8_t *pcm)
{
    constexpr DWORD size = STREAM_AUDIO_SAMPLES *
                           STREAM_AUDIO_BYTES_PER_FRAME;
    IMFMediaBuffer *buffer = nullptr;
    IMFSample *sample = nullptr;
    BYTE *destination = nullptr;
    const char *step = "AAC MFCreateMemoryBuffer";
    HRESULT hr = MFCreateMemoryBuffer(size, &buffer);
    if (SUCCEEDED(hr)) {
        step = "AAC IMFMediaBuffer::Lock";
        hr = buffer->Lock(&destination, nullptr, nullptr);
    }
    if (SUCCEEDED(hr)) {
        memcpy(destination, pcm, size);
        buffer->Unlock();
        destination = nullptr;
        hr = buffer->SetCurrentLength(size);
    }
    if (SUCCEEDED(hr)) {
        step = "AAC MFCreateSample";
        hr = MFCreateSample(&sample);
    }
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    LONGLONG sample_time = e->audio_pts_samples * 10000000LL /
                           STREAM_AUDIO_RATE;
    LONGLONG duration = STREAM_AUDIO_SAMPLES * 10000000LL /
                        STREAM_AUDIO_RATE;
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(sample_time);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    if (SUCCEEDED(hr)) {
        step = "AAC IMFTransform::ProcessInput";
        hr = e->audio_encoder->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            bool produced = false;
            hr = encoder_drain_audio_output(e, &produced);
            if (SUCCEEDED(hr)) {
                hr = e->audio_encoder->ProcessInput(0, sample, 0);
            }
        }
    }
    if (sample != nullptr) sample->Release();
    if (buffer != nullptr) buffer->Release();
    if (FAILED(hr)) {
        set_status("stream error: %s failed (0x%08lx)", step, (long)hr);
        return false;
    }
    e->audio_pts_samples += STREAM_AUDIO_SAMPLES;
    for (int i = 0; i < 8; ++i) {
        bool produced = false;
        hr = encoder_drain_audio_output(e, &produced);
        if (FAILED(hr) || !produced) {
            break;
        }
    }
    if (FAILED(hr)) {
        set_status("stream error: AAC ProcessOutput failed (0x%08lx)",
                   (long)hr);
        return false;
    }
    return true;
}

static bool encoder_write_audio(StreamEncoder *e, const StreamAudio *audio)
{
    if (audio == nullptr || audio->size == 0) {
        return true;
    }

    /* Audio shares the video clock: block arrival times place it on the
     * timeline. A count of encoded samples alone falls behind whenever a
     * block is dropped or the guest produces less than real time, and the
     * receiver then holds every video frame back by the difference. */
    constexpr int64_t kResyncSamples = 2 * STREAM_AUDIO_SAMPLES;
    int64_t block_samples =
        (int64_t)(audio->size / STREAM_AUDIO_BYTES_PER_FRAME);
    int64_t pending =
        (int64_t)(e->audio_pcm->len / STREAM_AUDIO_BYTES_PER_FRAME);
    int64_t block_start =
        (audio->arrival_us - e->timeline_start_us) * STREAM_AUDIO_RATE /
            1000000 -
        block_samples;
    if (block_start < 0) {
        return true; /* captured before the stream's first frame */
    }
    int64_t expected = block_start - pending;
    if (!e->audio_anchored) {
        e->audio_pts_samples = std::max<int64_t>(0, expected);
        e->audio_anchored = true;
    } else if (expected - e->audio_pts_samples > kResyncSamples) {
        e->audio_pts_samples = expected; /* audio was lost; skip ahead */
    } else if (e->audio_pts_samples - expected > kResyncSamples) {
        return true; /* audio is ahead of the clock; drop to catch up */
    }

    g_byte_array_append(e->audio_pcm, audio->data, (guint)audio->size);
    constexpr size_t block_size = STREAM_AUDIO_SAMPLES *
                                  STREAM_AUDIO_BYTES_PER_FRAME;
    while (e->audio_pcm->len >= block_size) {
        if (!encoder_write_audio_block(e, e->audio_pcm->data)) {
            return false;
        }
        g_byte_array_remove_range(e->audio_pcm, 0, (guint)block_size);
    }
    return true;
}

static bool encoder_write_frame(StreamEncoder *e, const StreamFrame *f)
{
    IMFMediaBuffer *buffer = nullptr;
    IMFSample *sample = nullptr;
    BYTE *pixels = nullptr;
    DWORD size = (DWORD)((size_t)e->out_w * e->out_h * 3 / 2);
    const char *step = "video input pool";
    HRESULT hr = S_OK;

    /* Frames sit on a fixed grid at the stream frame rate, on the same clock
     * as audio; a second frame within one interval is skipped. */
    const int fps = std::max(1, e->params.fps);
    const int64_t slot = (int64_t)std::llround(
        (double)(f->timestamp_us - e->timeline_start_us) * fps / 1000000.0);
    if (slot <= e->last_video_slot) {
        return true;
    }
    if (e->video_input_pool_active) {
        hr = e->video_input_pool->Acquire(&sample, &buffer);
        if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) {
            /* A system encoder can retain more look-ahead frames than the
             * pool depth. Allocate a buffer for this frame rather than
             * blocking the worker or dropping the frame. */
            hr = S_OK;
        } else if (FAILED(hr)) {
            fprintf(stderr,
                    "xemu stream: video input pool failed (0x%08lx); "
                    "using per-frame buffers\n",
                    (long)hr);
            e->video_input_pool->Shutdown();
            e->video_input_pool_active = false;
            hr = S_OK;
        }
    }
    if (SUCCEEDED(hr) && sample == nullptr) {
        step = "MFCreateMemoryBuffer";
        hr = MFCreateMemoryBuffer(size, &buffer);
        if (SUCCEEDED(hr)) {
            step = "MFCreateSample";
            hr = MFCreateSample(&sample);
        }
        if (SUCCEEDED(hr)) {
            step = "IMFSample::AddBuffer";
            hr = sample->AddBuffer(buffer);
        }
    }
    if (SUCCEEDED(hr)) {
        step = "IMFMediaBuffer::Lock";
        hr = buffer->Lock(&pixels, nullptr, nullptr);
    }
    if (SUCCEEDED(hr)) {
        bgra_to_nv12_scaled(e, f, pixels);
        buffer->Unlock();
        pixels = nullptr;
        step = "IMFMediaBuffer::SetCurrentLength";
        hr = buffer->SetCurrentLength(size);
    }
    LONGLONG duration = 10000000LL / fps;
    LONGLONG sample_time = slot * 10000000LL / fps;
    if (SUCCEEDED(hr)) {
        step = "IMFSample::SetSampleTime";
        hr = sample->SetSampleTime(sample_time);
    }
    if (SUCCEEDED(hr)) {
        step = "IMFSample::SetSampleDuration";
        hr = sample->SetSampleDuration(duration);
    }
    if (SUCCEEDED(hr)) {
        step = "IMFTransform::ProcessInput";
        hr = e->encoder->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            bool produced = false;
            hr = encoder_drain_output(e, &produced);
            if (SUCCEEDED(hr)) {
                hr = e->encoder->ProcessInput(0, sample, 0);
            }
        }
    }
    if (sample != nullptr)
        sample->Release();
    if (buffer != nullptr)
        buffer->Release();
    if (FAILED(hr)) {
        set_status("stream error: %s failed (0x%08lx)", step, (long)hr);
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        bool produced = false;
        step = "IMFTransform::ProcessOutput";
        hr = encoder_drain_output(e, &produced);
        if (FAILED(hr) || !produced) {
            break;
        }
    }
    if (FAILED(hr)) {
        set_status("stream error: %s failed (0x%08lx)", step, (long)hr);
        return false;
    }
    e->last_video_time = sample_time;
    e->last_video_slot = slot;
    ++e->pts;
    return true;
}

static void encoder_close(StreamEncoder *e)
{
    s_ready.store(false, std::memory_order_release);
    if (e->encoder != nullptr) {
        e->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        e->encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        for (;;) {
            bool produced = false;
            HRESULT hr = encoder_drain_output(e, &produced);
            if (FAILED(hr) || !produced) {
                break;
            }
        }
        if (e->video_input_pool != nullptr) {
            e->video_input_pool->Shutdown();
            e->video_input_pool_active = false;
        }
        e->encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        e->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        e->encoder->Release();
        e->encoder = nullptr;
    }
    if (e->video_output_buffer != nullptr) {
        e->video_output_buffer->Release();
        e->video_output_buffer = nullptr;
    }
    if (e->video_output_sample != nullptr) {
        e->video_output_sample->Release();
        e->video_output_sample = nullptr;
    }
    e->video_output_info_valid = false;
    if (e->video_input_pool != nullptr) {
        e->video_input_pool->Shutdown();
        /* FLUSH plus releasing the MFT must release every accepted input.
         * Keep Media Foundation and its work queues alive until all tracked
         * sample callbacks have returned their self-references. */
        e->video_input_pool->WaitForCallbacks();
        e->video_input_pool->Release();
        e->video_input_pool = nullptr;
    }
    e->video_input_pool_active = false;
    if (e->audio_encoder != nullptr) {
        e->audio_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        e->audio_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        for (;;) {
            bool produced = false;
            HRESULT hr = encoder_drain_audio_output(e, &produced);
            if (FAILED(hr) || !produced) {
                break;
            }
        }
        e->audio_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        e->audio_encoder->Release();
        e->audio_encoder = nullptr;
    }
    if (e->audio_pcm != nullptr) {
        g_byte_array_unref(e->audio_pcm);
        e->audio_pcm = nullptr;
    }
    delete e->mux;
    e->mux = nullptr;
    if (e->stream != nullptr) {
        e->stream->Flush();
        e->stream->Close();
        e->stream->Release();
        e->stream = nullptr;
    }
    if (e->mf_started) {
        MFShutdown();
        e->mf_started = false;
    }
}

static gpointer worker_main(gpointer)
{
    /* Software color conversion/encoding is optional background work. Keep it
     * from pre-empting the vCPU and D3D12 submission threads on Xbox. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool co_initialized = SUCCEEDED(co_hr);
    StreamEncoder encoder;
    g_mutex_lock(&s_lock);
    encoder.params = s_running_params;
    g_mutex_unlock(&s_lock);

    if (FAILED(co_hr)) {
        set_status("stream error: COM initialization failed (0x%08lx)",
                   (long)co_hr);
        s_active.store(false, std::memory_order_release);
        s_ready.store(false, std::memory_order_release);
        s_worker_run.store(false, std::memory_order_release);
    }

    while (SUCCEEDED(co_hr)) {
        g_mutex_lock(&s_lock);
        while (s_worker_run && g_queue_is_empty(s_queue) &&
               (encoder.encoder == nullptr ||
                g_queue_is_empty(s_audio_queue))) {
            g_cond_wait(&s_cond, &s_lock);
        }
        if (!s_worker_run && g_queue_is_empty(s_queue) &&
            (encoder.encoder == nullptr ||
             g_queue_is_empty(s_audio_queue))) {
            g_mutex_unlock(&s_lock);
            break;
        }
        auto *frame = static_cast<StreamFrame *>(g_queue_pop_head(s_queue));
        /* Take every waiting audio block: one per pass falls behind the
         * mixer and the queue starts dropping audio. */
        GQueue audio_blocks = G_QUEUE_INIT;
        if (encoder.encoder != nullptr) {
            while (!g_queue_is_empty(s_audio_queue)) {
                auto *block = static_cast<StreamAudio *>(
                    g_queue_pop_head(s_audio_queue));
                s_audio_queue_bytes -= block->size;
                g_queue_push_tail(&audio_blocks, block);
            }
        }
        g_mutex_unlock(&s_lock);
        if (frame != nullptr && encoder.encoder == nullptr &&
            !encoder_open(&encoder, frame->width, frame->height,
                          frame->timestamp_us)) {
            stream_frame_recycle(frame);
            while (!g_queue_is_empty(&audio_blocks)) {
                stream_audio_recycle(static_cast<StreamAudio *>(
                    g_queue_pop_head(&audio_blocks)));
            }
            g_mutex_lock(&s_lock);
            s_active.store(false, std::memory_order_release);
            s_ready.store(false, std::memory_order_release);
            s_worker_run.store(false, std::memory_order_release);
            g_mutex_unlock(&s_lock);
            break;
        }
        bool ok = true;
        while (!g_queue_is_empty(&audio_blocks)) {
            auto *block = static_cast<StreamAudio *>(
                g_queue_pop_head(&audio_blocks));
            ok = ok && encoder_write_audio(&encoder, block);
            stream_audio_recycle(block);
        }
        ok = ok && (frame == nullptr || encoder_write_frame(&encoder, frame));
        stream_frame_recycle(frame);
        if (!ok) {
            g_mutex_lock(&s_lock);
            s_active.store(false, std::memory_order_release);
            s_ready.store(false, std::memory_order_release);
            s_worker_run.store(false, std::memory_order_release);
            g_mutex_unlock(&s_lock);
            break;
        }
    }

    encoder_close(&encoder);
    g_mutex_lock(&s_lock);
    while (!g_queue_is_empty(s_queue)) {
        stream_frame_recycle_locked(
            static_cast<StreamFrame *>(g_queue_pop_head(s_queue)));
    }
    while (!g_queue_is_empty(s_audio_queue)) {
        stream_audio_recycle_locked(
            static_cast<StreamAudio *>(g_queue_pop_head(s_audio_queue)));
    }
    s_audio_queue_bytes = 0;
    s_active.store(false, std::memory_order_release);
    s_ready.store(false, std::memory_order_release);
    g_mutex_unlock(&s_lock);
    if (co_initialized) {
        CoUninitialize();
    }
    return nullptr;
}

static gpointer control_main(gpointer data)
{
    int port = GPOINTER_TO_INT(data);
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return nullptr;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((uint16_t)port);
    if (bind(sock, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        closesocket(sock);
        return nullptr;
    }
    DWORD timeout = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    while (s_control_run) {
        char command[64];
        int count = recv(sock, command, sizeof(command) - 1, 0);
        if (count <= 0) {
            continue;
        }
        command[count] = '\0';
        g_strstrip(command);
        if (g_ascii_strncasecmp(command, "START", 5) == 0) {
            xemu_stream_request_start();
        } else if (g_ascii_strncasecmp(command, "STOP", 4) == 0) {
            xemu_stream_request_stop();
        }
    }
    closesocket(sock);
    return nullptr;
}

static void control_start(int port)
{
    if (s_control == nullptr) {
        s_control_run = true;
        s_control_port_running = port;
        s_control = g_thread_new("xemu-stream-ctl", control_main,
                                 GINT_TO_POINTER(port));
    }
}

static void control_stop(void)
{
    if (s_control != nullptr) {
        s_control_run = false;
        g_thread_join(s_control);
        s_control = nullptr;
        s_control_port_running = 0;
    }
}

static void worker_start(void)
{
    StreamParams params{};
    snapshot_params(&params);
    g_mutex_lock(&s_lock);
    while (!g_queue_is_empty(s_audio_queue)) {
        stream_audio_recycle_locked(
            static_cast<StreamAudio *>(g_queue_pop_head(s_audio_queue)));
    }
    s_audio_queue_bytes = 0;
    s_running_params = params;
    s_frame_interval_us = 1000000 / s_running_params.fps;
    s_next_capture_us = 0;
    s_worker_run.store(true, std::memory_order_release);
    s_active.store(true, std::memory_order_release);
    s_ready.store(false, std::memory_order_release);
    s_start_failed.store(false, std::memory_order_release);
    g_mutex_unlock(&s_lock);
    ensure_audio_capture();
    set_status("starting stream to %s...", s_running_params.url);
    s_worker = g_thread_new("xemu-stream", worker_main, nullptr);
}

static void worker_stop(void)
{
    if (s_worker != nullptr) {
        g_mutex_lock(&s_lock);
        /* Stop producers before joining.  Otherwise the mixed-audio callback
         * can refill the queue as quickly as the worker drains it, preventing
         * the worker's empty-queue exit condition from ever becoming true. */
        s_active.store(false, std::memory_order_release);
        s_ready.store(false, std::memory_order_release);
        s_worker_run.store(false, std::memory_order_release);
        g_cond_broadcast(&s_cond);
        g_mutex_unlock(&s_lock);
        if (s_audio_capture_started) {
            xemu_stream_audio_stop();
            s_audio_capture_started = false;
        }
        g_thread_join(s_worker);
        s_worker = nullptr;
        if (!s_start_failed.load(std::memory_order_acquire)) {
            set_status("stream stopped");
        }
    }
}

extern "C" void xemu_stream_init(void)
{
    if (s_initialized.load(std::memory_order_acquire)) {
        return;
    }
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (!s_sync_initialized) {
        g_mutex_init(&s_lock);
        g_cond_init(&s_cond);
        s_sync_initialized = true;
    }
    s_queue = g_queue_new();
    s_frame_pool = g_queue_new();
    s_audio_queue = g_queue_new();
    s_audio_pool = g_queue_new();
    for (int i = 0; i < STREAM_AUDIO_POOL_MAX; ++i) {
        auto *audio = g_new0(StreamAudio, 1);
        audio->data = static_cast<uint8_t *>(
            g_malloc(STREAM_AUDIO_BLOCK_HINT));
        audio->capacity = STREAM_AUDIO_BLOCK_HINT;
        g_queue_push_tail(s_audio_pool, audio);
    }
    s_worker_run.store(false, std::memory_order_relaxed);
    s_active.store(false, std::memory_order_relaxed);
    s_ready.store(false, std::memory_order_relaxed);
    s_control_run.store(false, std::memory_order_relaxed);
    s_start_failed.store(false, std::memory_order_relaxed);
    s_remote_active.store(false, std::memory_order_relaxed);
    g_mutex_lock(&s_lock);
    s_remote_override = false;
    g_mutex_unlock(&s_lock);
    g_strlcpy(s_status, "stream idle", sizeof(s_status));
    s_initialized.store(true, std::memory_order_release);
}

extern "C" void xemu_stream_shutdown(void)
{
    if (!s_initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    s_active.store(false, std::memory_order_release);
    s_ready.store(false, std::memory_order_release);
    control_stop();
    worker_stop();
    if (s_audio_capture_started) {
        xemu_stream_audio_stop();
        s_audio_capture_started = false;
    }
    g_queue_free_full(s_queue, reinterpret_cast<GDestroyNotify>(stream_frame_free));
    s_queue = nullptr;
    g_queue_free_full(s_frame_pool,
                      reinterpret_cast<GDestroyNotify>(stream_frame_free));
    s_frame_pool = nullptr;
    g_queue_free_full(s_audio_queue,
                      reinterpret_cast<GDestroyNotify>(stream_audio_free));
    s_audio_queue = nullptr;
    g_queue_free_full(s_audio_pool,
                      reinterpret_cast<GDestroyNotify>(stream_audio_free));
    s_audio_pool = nullptr;
    s_audio_queue_bytes = 0;
    s_remote_active.store(false, std::memory_order_release);
    s_start_failed.store(false, std::memory_order_release);
    g_mutex_lock(&s_lock);
    s_remote_override = false;
    g_mutex_unlock(&s_lock);
    WSACleanup();
}

extern "C" void xemu_stream_request_start(void)
{
    if (!s_initialized.load(std::memory_order_acquire)) {
        return;
    }
    g_mutex_lock(&s_lock);
    s_remote_override = false;
    s_start_failed.store(false, std::memory_order_release);
    s_remote_active.store(true, std::memory_order_release);
    g_mutex_unlock(&s_lock);
}

extern "C" void xemu_stream_request_start_with_params(
    const char *url, int bitrate_kbps, int fps)
{
    if (!s_initialized.load(std::memory_order_acquire) || url == nullptr ||
        url[0] == '\0') {
        return;
    }

    StreamParams params{};
    snapshot_config_params(&params);
    g_strlcpy(params.url, url, sizeof(params.url));
    if (bitrate_kbps >= 100 && bitrate_kbps <= 100000) {
        params.bitrate_kbps = bitrate_kbps;
    }
    if (fps >= 1 && fps <= 120) {
        params.fps = fps;
    }
    g_mutex_lock(&s_lock);
    s_remote_params = params;
    s_remote_override = true;
    s_start_failed.store(false, std::memory_order_release);
    s_remote_active.store(true, std::memory_order_release);
    g_mutex_unlock(&s_lock);
}

extern "C" void xemu_stream_request_stop(void)
{
    if (!s_initialized.load(std::memory_order_acquire)) {
        return;
    }
    g_mutex_lock(&s_lock);
    s_remote_active.store(false, std::memory_order_release);
    s_remote_override = false;
    g_mutex_unlock(&s_lock);
}

extern "C" bool xemu_stream_is_active(void)
{
    return s_active.load(std::memory_order_acquire);
}
extern "C" bool xemu_stream_is_ready(void)
{
    return s_ready.load(std::memory_order_acquire);
}
extern "C" bool xemu_stream_is_supported(void) { return true; }

extern "C" bool xemu_stream_get_output_size(int *width, int *height)
{
    if (width == nullptr || height == nullptr ||
        !s_ready.load(std::memory_order_acquire)) {
        return false;
    }
    g_mutex_lock(&s_lock);
    *width = s_output_width;
    *height = s_output_height;
    bool valid = *width > 0 && *height > 0;
    g_mutex_unlock(&s_lock);
    return valid;
}

extern "C" bool xemu_stream_wants_frame(void)
{
    bool wants;
    int64_t now;

    if (!s_active.load(std::memory_order_acquire)) {
        return false;
    }
    now = g_get_monotonic_time();
    g_mutex_lock(&s_lock);
    /* Captures follow a fixed grid at the stream frame rate, tolerating a
     * quarter interval of present jitter so a frame that is slightly early
     * is not skipped for a whole interval. */
    int64_t tolerance = s_frame_interval_us / 4;
    wants = s_active.load(std::memory_order_relaxed) && s_queue != nullptr &&
            (s_frame_interval_us <= 0 ||
             now + tolerance >= s_next_capture_us);
    if (wants && s_frame_interval_us > 0) {
        s_next_capture_us = std::max(s_next_capture_us + s_frame_interval_us,
                                     now - tolerance);
    }
    g_mutex_unlock(&s_lock);
    return wants;
}

extern "C" void xemu_stream_submit_bgra(const uint8_t *bgra, int width,
                                          int height, int pitch)
{
    if (!s_active.load(std::memory_order_acquire) || bgra == nullptr ||
        width <= 0 || height <= 0 || pitch < width * 4) {
        return;
    }
    int64_t now = g_get_monotonic_time();
    g_mutex_lock(&s_lock);
    if (!s_active || g_queue_get_length(s_queue) >= STREAM_QUEUE_MAX) {
        g_mutex_unlock(&s_lock);
        return;
    }
    g_mutex_unlock(&s_lock);

    auto *frame = stream_frame_acquire((size_t)width * height * 4);
    frame->width = width;
    frame->height = height;
    frame->pitch = width * 4;
    frame->timestamp_us = now;
    for (int y = 0; y < height; ++y) {
        memcpy(frame->data + (size_t)y * width * 4,
               bgra + (size_t)y * pitch, (size_t)width * 4);
    }
    g_mutex_lock(&s_lock);
    if (!s_active || g_queue_get_length(s_queue) >= STREAM_QUEUE_MAX) {
        g_mutex_unlock(&s_lock);
        stream_frame_recycle(frame);
        return;
    }
    g_queue_push_tail(s_queue, frame);
    g_cond_signal(&s_cond);
    g_mutex_unlock(&s_lock);
}

extern "C" void xemu_stream_submit_bgra_borrowed(
    const uint8_t *bgra, int width, int height, int pitch,
    int64_t capture_us, XemuStreamFrameRelease release, void *opaque)
{
    if (!s_active.load(std::memory_order_acquire) || bgra == nullptr ||
        width <= 0 || height <= 0 || pitch < width * 4) {
        if (release != nullptr) {
            release(opaque);
        }
        return;
    }

    auto *frame = stream_frame_acquire_borrowed(bgra, release, opaque);
    frame->width = width;
    frame->height = height;
    frame->pitch = pitch;
    frame->timestamp_us = capture_us > 0 ? capture_us : g_get_monotonic_time();

    /* Only the newest frame waits for the encoder; an older waiting frame
     * is released rather than encoded late. */
    g_mutex_lock(&s_lock);
    if (!s_active.load(std::memory_order_relaxed) || s_queue == nullptr) {
        stream_frame_recycle_locked(frame);
        g_mutex_unlock(&s_lock);
        return;
    }
    while (g_queue_get_length(s_queue) >= STREAM_QUEUE_MAX) {
        stream_frame_recycle_locked(
            static_cast<StreamFrame *>(g_queue_pop_head(s_queue)));
    }
    g_queue_push_tail(s_queue, frame);
    g_cond_signal(&s_cond);
    g_mutex_unlock(&s_lock);
}

extern "C" const char *xemu_stream_get_status(void)
{
    static thread_local char status[sizeof(s_status)];
    g_mutex_lock(&s_lock);
    g_strlcpy(status, s_status, sizeof(status));
    g_mutex_unlock(&s_lock);
    return status;
}

extern "C" void xemu_stream_poll(void)
{
    if (!s_initialized.load(std::memory_order_acquire)) {
        return;
    }
    int port = g_config.display.stream.control_port;
    if (port < 1 || port > 65535) {
        port = 26961;
    }
    if (g_config.display.stream.remote_start) {
        if (s_control != nullptr && s_control_port_running != port) {
            control_stop();
        }
        control_start(port);
    } else {
        control_stop();
    }

    if (s_worker != nullptr && !s_worker_run && !s_active) {
        if (s_audio_capture_started) {
            xemu_stream_audio_stop();
            s_audio_capture_started = false;
        }
        g_thread_join(s_worker);
        s_worker = nullptr;
        s_start_failed.store(true, std::memory_order_release);
        s_remote_active.store(false, std::memory_order_release);
    }

    const bool desired = g_config.display.stream.broadcast_enabled ||
                         s_remote_active.load(std::memory_order_acquire);
    const bool active = s_active.load(std::memory_order_acquire);
    bool start_failed = s_start_failed.load(std::memory_order_acquire);

    /* This is the normal per-frame path when streaming is disabled.  Remote
     * state is atomic, so it needs neither the queue lock nor a 512-byte URL
     * snapshot.  Clearing a previous failure while disabled preserves the
     * retry behavior when broadcast is enabled again. */
    if (!desired && !active && s_worker == nullptr) {
        if (start_failed) {
            s_start_failed.store(false, std::memory_order_release);
        }
        return;
    }

    if (desired && !active && s_worker == nullptr && !start_failed) {
        worker_start();
        return;
    }
    if (!desired && (active || s_worker != nullptr)) {
        worker_stop();
        return;
    }

    StreamParams current{};
    bool have_current = false;
    if (active && s_worker != nullptr) {
        snapshot_params(&current);
        have_current = true;
    }
    if (have_current && params_differ(current, s_running_params)) {
        worker_stop();
        s_start_failed.store(false, std::memory_order_release);
        return;
    }

    start_failed = s_start_failed.load(std::memory_order_acquire);
    if (start_failed) {
        if (!desired) {
            s_start_failed.store(false, std::memory_order_release);
        } else {
            if (!have_current) {
                snapshot_params(&current);
            }
            if (params_differ(current, s_running_params)) {
                s_start_failed.store(false, std::memory_order_release);
            }
        }
    }
}
