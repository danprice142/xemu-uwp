//
// xemu User Interface
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "common.hh"
#include "game-library.hh"
#include "actions.hh"
#include "animation.hh"
#include "font-manager.hh"
#include "input-manager.hh"
#include "main-menu.hh"
#include "scene-manager.hh"
#include "viewport-manager.hh"
#include "xemu-hud.h"
#include "IconsFontAwesome6.h"
#include "../xemu-dsu.h"
#include "../xemu-game-library.h"
#include "../xemu-notifications.h"
#include "../xemu-screenscraper.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
/* mingw-w64 hides this type once windows.h has declared the GDI bitmaps. */
#if defined(__MINGW32__) && defined(_WINGDI_)
typedef struct {
    GUID guidMajorType;
    GUID guidSubtype;
} MFT_REGISTER_TYPE_INFO;
#endif
#include <mftransform.h>
#include <mfidl.h>
#include <mfreadwrite.h>

/* Exported by mfplat but missing from the MinGW headers. */
extern "C" HRESULT WINAPI MFCreateMFByteStreamOnStream(IStream *stream,
                                                       IMFByteStream **bytes);
#endif

namespace {

namespace ss = xemu::screenscraper;

constexpr size_t kMaxSmallTextures = 96;
constexpr size_t kMaxPendingDecodes = 48;
constexpr int kUploadsPerFrame = 2;
constexpr uint64_t kLargeTextureIdleFrames = 90;
constexpr double kBackgroundDelay = 0.15;
constexpr double kBackgroundFade = 0.6;
constexpr double kArtFade = 0.25;
constexpr double kSelectionRepeat = 2.0;
constexpr double kVideoDelay = 0.6;
constexpr double kSelectionGrow = 0.18;
constexpr int kMaxImageDimension = 8192;
constexpr double kTau = 6.283185307179586;
// A launch held for shaders shows its panel only once it takes this long,
// and gives up on a shader list that does not load.
constexpr double kPrepareShowDelay = 1.0;
constexpr double kPrepareLoadTimeout = 15.0;

struct DecodeJob {
    std::string key;
    std::string path;
    int max_width;
    int max_height;
};

struct DecodeResult {
    std::string key;
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
};

// Area-averaging reduction of tightly packed RGBA8 pixels (dw <= sw, dh <= sh).
void ShrinkRgba(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw,
                int dh)
{
    std::vector<uint32_t> sum((size_t)dw * 4);
    std::vector<uint32_t> count(dw);
    int sy = 0;

    for (int y = 0; y < dh; y++) {
        int y_end = std::min(sh, std::max(sy + 1, (int)((int64_t)(y + 1) *
                                                         sh / dh)));
        std::fill(sum.begin(), sum.end(), 0);
        std::fill(count.begin(), count.end(), 0);
        for (; sy < y_end; sy++) {
            const uint8_t *row = src + (size_t)sy * sw * 4;
            int sx = 0;
            for (int x = 0; x < dw; x++) {
                int x_end = std::min(sw, std::max(sx + 1,
                                                  (int)((int64_t)(x + 1) *
                                                        sw / dw)));
                for (; sx < x_end; sx++) {
                    const uint8_t *p = row + (size_t)sx * 4;
                    sum[x * 4 + 0] += p[0];
                    sum[x * 4 + 1] += p[1];
                    sum[x * 4 + 2] += p[2];
                    sum[x * 4 + 3] += p[3];
                    count[x]++;
                }
            }
        }
        uint8_t *out = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++) {
            uint32_t n = count[x] ? count[x] : 1;
            for (int c = 0; c < 4; c++) {
                out[x * 4 + c] = (uint8_t)((sum[x * 4 + c] + n / 2) / n);
            }
        }
    }
}

bool DecodeImageFile(const DecodeJob &job, DecodeResult *out)
{
    gchar *data = nullptr;
    gsize size = 0;

    if (!g_file_get_contents(job.path.c_str(), &data, &size, nullptr)) {
        return false;
    }
    if (size == 0 || size > 32 * 1024 * 1024) {
        g_free(data);
        return false;
    }

    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load_thread(0);
    stbi_uc *pixels = stbi_load_from_memory((const stbi_uc *)data, (int)size,
                                            &width, &height, &channels, 4);
    g_free(data);
    if (pixels == nullptr || width <= 0 || height <= 0 ||
        width > kMaxImageDimension || height > kMaxImageDimension) {
        stbi_image_free(pixels);
        return false;
    }

    float scale = std::min({ 1.0f, (float)job.max_width / width,
                             (float)job.max_height / height });
    int dw = std::max(1, (int)lroundf(width * scale));
    int dh = std::max(1, (int)lroundf(height * scale));
    out->pixels.resize((size_t)dw * dh * 4);
    if (dw == width && dh == height) {
        memcpy(out->pixels.data(), pixels, out->pixels.size());
    } else {
        ShrinkRgba(pixels, width, height, out->pixels.data(), dw, dh);
    }
    stbi_image_free(pixels);
    out->width = dw;
    out->height = dh;
    return true;
}

// Decodes and shrinks artwork off the UI thread.
class ArtDecoder
{
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::deque<DecodeJob> m_jobs;
    std::deque<DecodeResult> m_results;
    bool m_stop = false;

    void Run()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (true) {
            m_cond.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop) {
                return;
            }
            DecodeJob job = std::move(m_jobs.front());
            m_jobs.pop_front();
            lock.unlock();

            DecodeResult result;
            result.key = job.key;
            if (!DecodeImageFile(job, &result)) {
                result.pixels.clear();
            }

            lock.lock();
            if (!m_stop) {
                m_results.push_back(std::move(result));
            }
        }
    }

public:
    ~ArtDecoder() { Stop(); }

    void Start()
    {
        if (!m_thread.joinable()) {
            m_stop = false;
            m_thread = std::thread([this] { Run(); });
        }
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
            m_jobs.clear();
            m_results.clear();
        }
        m_cond.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    size_t Pending()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_jobs.size();
    }

    void Submit(DecodeJob job, bool urgent)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (urgent) {
                m_jobs.push_front(std::move(job));
            } else {
                m_jobs.push_back(std::move(job));
            }
        }
        m_cond.notify_one();
    }

    bool TakeResult(DecodeResult *out)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_results.empty()) {
            return false;
        }
        *out = std::move(m_results.front());
        m_results.pop_front();
        return true;
    }
};

// Plays a video file without audio on a worker thread, publishing frames as
// RGBA8. The file is read through GLib so the same paths as the artwork work.
class VideoBackground
{
    std::string m_path;
    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    std::mutex m_mutex;
    std::vector<uint8_t> m_frame;
    int m_width = 0;
    int m_height = 0;
    uint64_t m_generation = 0;
    uint64_t m_taken = 0;

#ifdef _WIN32
    template <typename T> static void SafeRelease(T **p)
    {
        if (*p != nullptr) {
            (*p)->Release();
            *p = nullptr;
        }
    }

    void Publish(const uint8_t *scan0, LONG pitch, UINT32 width,
                 UINT32 height)
    {
        std::vector<uint8_t> rgba((size_t)width * height * 4);
        for (UINT32 y = 0; y < height; y++) {
            const uint8_t *src = scan0 + (ptrdiff_t)y * pitch;
            uint8_t *dst = rgba.data() + (size_t)y * width * 4;
            for (UINT32 x = 0; x < width; x++) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frame = std::move(rgba);
        m_width = (int)width;
        m_height = (int)height;
        m_generation++;
    }

    void Decode(std::string path)
    {
        static std::once_flag startup_once;
        static HRESULT startup = E_FAIL;
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::call_once(startup_once, [] {
            startup = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        });

        gchar *data = nullptr;
        gsize size = 0;
        IStream *stream = nullptr;
        IMFByteStream *bytes = nullptr;
        IMFAttributes *attributes = nullptr;
        IMFSourceReader *reader = nullptr;
        IMFMediaType *requested = nullptr;
        IMFMediaType *current = nullptr;
        UINT32 width = 0, height = 0;
        LONG default_stride = 0;
        LONGLONG previous_time = -1;
        auto previous_wall = std::chrono::steady_clock::now();

        if (FAILED(startup) ||
            !g_file_get_contents(path.c_str(), &data, &size, nullptr) ||
            size == 0 || size > 128 * 1024 * 1024 ||
            FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
            goto done;
        }
        {
            ULONG written = 0;
            LARGE_INTEGER origin = {};
            if (FAILED(stream->Write(data, (ULONG)size, &written)) ||
                written != size ||
                FAILED(stream->Seek(origin, STREAM_SEEK_SET, nullptr))) {
                goto done;
            }
        }
        g_free(data);
        data = nullptr;

        if (FAILED(MFCreateMFByteStreamOnStream(stream, &bytes)) ||
            FAILED(MFCreateAttributes(&attributes, 1)) ||
            FAILED(attributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
            FAILED(MFCreateSourceReaderFromByteStream(bytes, attributes,
                                                      &reader))) {
            goto done;
        }
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        if (FAILED(MFCreateMediaType(&requested)) ||
            FAILED(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
            FAILED(reader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, requested)) ||
            FAILED(reader->GetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current)) ||
            FAILED(MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &width,
                                      &height)) ||
            width == 0 || height == 0 || width > 4096 || height > 4096) {
            goto done;
        }
        {
            UINT32 stride = 0;
            default_stride = SUCCEEDED(current->GetUINT32(
                                 MF_MT_DEFAULT_STRIDE, &stride)) ?
                                 (LONG)stride :
                                 (LONG)(width * 4);
        }

        while (!m_stop.load()) {
            DWORD flags = 0;
            LONGLONG time = 0;
            IMFSample *sample = nullptr;
            if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                          0, nullptr, &flags, &time,
                                          &sample))) {
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                PROPVARIANT position;
                PropVariantInit(&position);
                position.vt = VT_I8;
                position.hVal.QuadPart = 0;
                const GUID time_format = {};
                reader->SetCurrentPosition(time_format, position);
                previous_time = -1;
                SafeRelease(&sample);
                continue;
            }
            if (sample == nullptr) {
                continue;
            }

            // Pace frames by their timestamps (100 ns units).
            if (previous_time >= 0 && time > previous_time) {
                auto due = previous_wall + std::chrono::microseconds(
                                               std::min<LONGLONG>(
                                                   (time - previous_time) / 10,
                                                   200000));
                while (!m_stop.load() &&
                       std::chrono::steady_clock::now() < due) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            previous_time = time;
            previous_wall = std::chrono::steady_clock::now();

            IMFMediaBuffer *buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                IMF2DBuffer *buffer2d = nullptr;
                BYTE *scan0 = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) &&
                    SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
                    Publish(scan0, pitch, width, height);
                    buffer2d->Unlock2D();
                } else {
                    BYTE *base = nullptr;
                    DWORD length = 0;
                    if (SUCCEEDED(buffer->Lock(&base, nullptr, &length))) {
                        LONG abs_stride = std::abs(default_stride);
                        if (length >= (DWORD)abs_stride * height) {
                            // A negative stride stores the image bottom-up.
                            const BYTE *top = default_stride < 0 ?
                                                  base + (size_t)abs_stride *
                                                             (height - 1) :
                                                  base;
                            Publish(top, default_stride, width, height);
                        }
                        buffer->Unlock();
                    }
                }
                SafeRelease(&buffer2d);
                SafeRelease(&buffer);
            }
            SafeRelease(&sample);
        }

    done:
        g_free(data);
        SafeRelease(&current);
        SafeRelease(&requested);
        SafeRelease(&reader);
        SafeRelease(&attributes);
        SafeRelease(&bytes);
        SafeRelease(&stream);
        if (SUCCEEDED(co)) {
            CoUninitialize();
        }
    }
#else
    void Decode(std::string path) { (void)path; }
#endif

public:
    ~VideoBackground() { SetPath(std::string()); }

    const std::string &Path() const { return m_path; }

    void SetPath(const std::string &path)
    {
        if (path == m_path) {
            return;
        }
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_frame.clear();
            m_width = m_height = 0;
            m_generation = m_taken = 0;
        }
        m_path = path;
        m_stop = false;
        if (!path.empty()) {
            m_thread = std::thread([this, path] { Decode(path); });
        }
    }

    // Moves out the newest frame if it has not been taken yet.
    bool TakeFrame(std::vector<uint8_t> *rgba, int *width, int *height)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_generation == m_taken || m_frame.empty()) {
            return false;
        }
        m_taken = m_generation;
        *rgba = std::move(m_frame);
        m_frame.clear();
        *width = m_width;
        *height = m_height;
        return true;
    }
};

struct ArtTexture {
    enum class State { Idle, Queued, Ready, Failed };
    State state = State::Idle;
    void *resource = nullptr;
    uint64_t texture_id = 0;
    int width = 0;
    int height = 0;
    uint64_t last_used = 0;
    double ready_time = 0.0;
    bool large = false;
};

struct LibraryGame {
    std::string token;
    uint64_t launch_id = 0;
    std::string label;
    std::string cover_path;
    std::string large_art_path;
    std::string background_path;
    std::string video_path;
    std::string logo_path;
    std::string synopsis;
    std::string developer;
    std::string publisher;
    std::string genre;
    std::string players;
    std::string release_date;
    std::string rating;
    uint32_t title_id = 0;
};

std::string ToString(const char *text)
{
    return text != nullptr ? text : "";
}

float EaseOutCubic(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

ImU32 WithAlpha(ImU32 color, float alpha)
{
    uint32_t a = (color >> IM_COL32_A_SHIFT) & 0xff;
    a = (uint32_t)std::clamp(a * alpha, 0.0f, 255.0f);
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// UV range that crops the image to fill a rect of the given aspect.
void CropUv(float image_aspect, float rect_aspect, ImVec2 *uv0, ImVec2 *uv1)
{
    *uv0 = ImVec2(0, 0);
    *uv1 = ImVec2(1, 1);
    if (image_aspect > rect_aspect) {
        float u = rect_aspect / image_aspect;
        uv0->x = (1.0f - u) * 0.5f;
        uv1->x = 1.0f - uv0->x;
    } else if (image_aspect < rect_aspect) {
        float v = image_aspect / rect_aspect;
        uv0->y = (1.0f - v) * 0.5f;
        uv1->y = 1.0f - uv0->y;
    }
}

// Width over height of a tile showing the given box art style.
float TileAspect(int style)
{
    switch (style) {
    case ss::kBoxArt3D:
        return 0.86f;
    case ss::kBoxArtMixV1:
    case ss::kBoxArtMixV2:
    case ss::kBoxArtScreenshot:
        return 1.33f;
    case ss::kBoxArtFanArt:
        return 1.78f;
    case ss::kBoxArtLogo:
        return 2.0f;
    case ss::kBoxArtDisc:
        return 1.0f;
    default:
        return 0.71f;
    }
}

// Styles that are usually cut out on transparency rather than rectangular.
bool IsCutoutStyle(int style)
{
    return style == ss::kBoxArt3D || style == ss::kBoxArtMixV1 ||
           style == ss::kBoxArtMixV2 || style == ss::kBoxArtLogo ||
           style == ss::kBoxArtDisc;
}

const char *MonthName(int month)
{
    static const char *const kMonths[] = { "Jan", "Feb", "Mar", "Apr",
                                           "May", "Jun", "Jul", "Aug",
                                           "Sep", "Oct", "Nov", "Dec" };
    return month >= 1 && month <= 12 ? kMonths[month - 1] : nullptr;
}

// "2002-10-29" -> "29 Oct 2002"; partial dates keep what is known.
std::string FormatReleaseDate(const std::string &date)
{
    int year = 0, month = 0, day = 0;
    int fields = sscanf(date.c_str(), "%d-%d-%d", &year, &month, &day);
    if (fields <= 0 || year < 1970 || year > 2100) {
        return date;
    }
    const char *name = MonthName(month);
    if (fields >= 3 && name != nullptr && day >= 1 && day <= 31) {
        return std::to_string(day) + " " + name + " " + std::to_string(year);
    }
    if (fields >= 2 && name != nullptr) {
        return std::string(name) + " " + std::to_string(year);
    }
    return std::to_string(year);
}

std::string FormatPlayers(const std::string &players)
{
    if (players.empty()) {
        return players;
    }
    return players == "1" ? "1 player" : players + " players";
}

// Five-star value of a score out of 20 (or out of 5 if small).
float RatingStars(const std::string &rating)
{
    if (rating.empty()) {
        return -1.0f;
    }
    char *end = nullptr;
    double score = g_ascii_strtod(rating.c_str(), &end);
    if (end == rating.c_str()) {
        return -1.0f;
    }
    if (score > 5.0) {
        score /= 4.0;
    }
    return (float)std::clamp(score, 0.0, 5.0);
}

void DrawStars(ImDrawList *dl, ImVec2 pos, float size, float stars,
               float alpha)
{
    float outer = size * 0.5f;
    float inner = outer * 0.45f;
    float gap = size * 0.22f;
    int filled = (int)lroundf(stars);
    for (int star = 0; star < 5; star++) {
        ImVec2 centre(pos.x + outer + star * (size + gap), pos.y + outer);
        ImVec2 points[10];
        for (int p = 0; p < 10; p++) {
            float radius = p % 2 == 0 ? outer : inner;
            float angle = (float)(-kTau / 4 + p * kTau / 10);
            points[p] = ImVec2(centre.x + cosf(angle) * radius,
                               centre.y + sinf(angle) * radius);
        }
        ImU32 color = star < filled ? IM_COL32(255, 194, 46, 255) :
                                      IM_COL32(120, 125, 135, 200);
        dl->AddConcavePolyFilled(points, 10, WithAlpha(color, alpha));
    }
}

// Draws word-wrapped text limited to max_lines; the last line is ellipsized.
float DrawWrappedLines(ImDrawList *dl, ImFont *font, float size, ImVec2 pos,
                       float wrap, int max_lines, ImU32 color,
                       const std::string &text)
{
    const char *begin = text.c_str();
    const char *end = begin + text.size();
    float scale = size / font->FontSize;
    float line_height = size * 1.15f;
    float y = pos.y;
    for (int line = 0; line < max_lines && begin < end; line++) {
        while (begin < end && (*begin == ' ' || *begin == '\n')) {
            begin++;
        }
        const char *newline = (const char *)memchr(begin, '\n', end - begin);
        const char *limit = newline ? newline : end;
        const char *stop =
            font->CalcWordWrapPositionA(scale, begin, limit, wrap);
        if (stop == begin) {
            stop = begin + 1;
        }
        bool last = line == max_lines - 1 && stop < end;
        if (last) {
            std::string clipped(begin, stop);
            while (!clipped.empty() && clipped.back() == ' ') {
                clipped.pop_back();
            }
            clipped += "\xE2\x80\xA6";
            dl->AddText(font, size, ImVec2(pos.x, y), color, clipped.c_str());
        } else {
            dl->AddText(font, size, ImVec2(pos.x, y), color, begin, stop);
        }
        y += line_height;
        begin = stop;
    }
    return y - pos.y;
}

} // namespace

class GameLibraryView
{
public:
    std::vector<LibraryGame> m_games;
    uint64_t m_generation = UINT64_MAX;
    int m_selected = 0;
    std::string m_selected_token;
    double m_selection_time = 0;

    ArtDecoder m_decoder;
    std::unordered_map<std::string, ArtTexture> m_art;
    uint64_t m_frame = 0;

    std::string m_background_key;
    std::string m_previous_background_key;
    double m_background_time = -1e9;

    VideoBackground m_video;
    void *m_video_resource = nullptr;
    uint64_t m_video_texture = 0;
    int m_video_width = 0;
    int m_video_height = 0;
    bool m_video_ready = false;
    double m_video_time = 0;

    float m_scroll = 0;
    int m_first = 0;

    uint64_t m_announced_id = 0;
    double m_announced_time = -1e9;

    EasingAnimation m_fade{ 0.3f, 0.25f };
    bool m_active = false;
    bool m_closing = false;
    bool m_was_running = false;
    double m_show_time = 0;

    // A launched game held at reset while its shaders precompile.
    bool m_preparing = false;
    double m_prepare_time = 0;
    std::string m_prepare_name;
    NV2AShaderPrecompileStatus m_prepare_status = {};

    void Show()
    {
        m_active = true;
        m_closing = false;
        m_fade.EaseIn();
        m_show_time = ImGui::GetTime();
        m_selection_time = m_show_time;
        m_generation = UINT64_MAX;
        m_was_running = runstate_is_running();
        if (m_was_running) {
            vm_stop(RUN_STATE_PAUSED);
        }
        xemu_game_library_startup_pending = false;
        m_decoder.Start();
    }

    void BeginClose()
    {
        if (m_preparing) {
            // Leaving early still starts the launched game; the rest of
            // its shaders compile in the background.
            m_preparing = false;
            nv2a_shader_precompile_set_foreground(false);
            if (!runstate_is_running()) {
                vm_start();
            }
        }
        if (!m_closing) {
            m_closing = true;
            m_fade.EaseOut();
            m_video.SetPath(std::string());
        }
    }

    void ReleaseVideoTexture()
    {
        if (m_video_resource != nullptr) {
            xemu_hud_release_d3d12_texture(m_video_resource);
        }
        m_video_resource = nullptr;
        m_video_texture = 0;
        m_video_width = m_video_height = 0;
        m_video_ready = false;
    }

    void Teardown()
    {
        m_decoder.Stop();
        m_video.SetPath(std::string());
        ReleaseVideoTexture();
        for (auto &entry : m_art) {
            if (entry.second.resource != nullptr) {
                xemu_hud_release_d3d12_texture(entry.second.resource);
            }
        }
        m_art.clear();
        m_background_key.clear();
        m_previous_background_key.clear();
        m_active = false;
    }

    const LibraryGame *Selected() const
    {
        if (m_selected < 0 || m_selected >= (int)m_games.size()) {
            return nullptr;
        }
        return &m_games[m_selected];
    }

    void Select(int index)
    {
        if (m_games.empty()) {
            return;
        }
        index = std::clamp(index, 0, (int)m_games.size() - 1);
        if (index != m_selected) {
            m_selected = index;
            m_selection_time = ImGui::GetTime();
        }
        m_selected_token = m_games[m_selected].token;
    }

    // Follows the game a network frontend highlights, and reports the local
    // selection.
    void SyncRemoteSelection(double now)
    {
        if (!xemu_dsu_is_active()) {
            return;
        }
        uint64_t requested = 0;
        if (xemu_dsu_take_app_selection(&requested)) {
            for (size_t i = 0; i < m_games.size(); i++) {
                if (m_games[i].launch_id == requested) {
                    Select((int)i);
                    break;
                }
            }
            // Acknowledge even an unchanged selection.
            m_announced_id = 0;
        }

        const LibraryGame *game = Selected();
        if (game == nullptr || game->launch_id == 0) {
            return;
        }
        if (game->launch_id != m_announced_id ||
            now - m_announced_time >= kSelectionRepeat) {
            m_announced_id = game->launch_id;
            m_announced_time = now;
            xemu_dsu_send_app_selection(xemu_game_library_catalogue_revision(),
                                        game->launch_id);
        }
    }

    void SyncGames()
    {
        uint64_t generation = xemu_game_library_generation();
        if (generation == m_generation) {
            return;
        }

        size_t count = 0;
        bool available = false;
        XemuGameLibraryEntry *entries =
            xemu_game_library_snapshot_try(&count, &available);
        if (!available) {
            // Busy publishing an update: keep the current list and retry.
            return;
        }
        m_games.clear();
        m_games.reserve(count);
        for (size_t i = 0; i < count; i++) {
            const XemuGameLibraryEntry &e = entries[i];
            LibraryGame game;
            game.token = ToString(e.token);
            game.launch_id = xemu_game_library_launch_id(e.token);
            game.label = ToString(e.label);
            game.cover_path = ToString(e.cover_path);
            game.large_art_path = ToString(e.large_art_path);
            game.background_path = ToString(e.background_path);
            game.video_path = ToString(e.video_path);
            game.logo_path = ToString(e.logo_path);
            game.synopsis = ToString(e.synopsis);
            game.developer = ToString(e.developer);
            game.publisher = ToString(e.publisher);
            game.genre = ToString(e.genre);
            game.players = ToString(e.players);
            game.release_date = ToString(e.release_date);
            game.rating = ToString(e.rating);
            game.title_id = e.title_id;
            m_games.push_back(std::move(game));
        }
        xemu_game_library_free_snapshot(entries, count);
        m_generation = generation;

        std::stable_sort(m_games.begin(), m_games.end(),
                         [](const LibraryGame &a, const LibraryGame &b) {
                             return g_ascii_strcasecmp(a.label.c_str(),
                                                       b.label.c_str()) < 0;
                         });

        int selected = 0;
        for (size_t i = 0; i < m_games.size(); i++) {
            if (m_games[i].token == m_selected_token) {
                selected = (int)i;
                break;
            }
        }
        m_selected = m_games.empty() ? 0 : selected;
        if (!m_games.empty()) {
            m_selected_token = m_games[m_selected].token;
        }
    }

    static std::string ArtKey(const std::string &path, int max_width,
                              int max_height)
    {
        return path + "|" + std::to_string(max_width) + "x" +
               std::to_string(max_height);
    }

    // Returns the texture when ready; otherwise schedules the decode.
    ArtTexture *RequestArt(const std::string &path, int max_width,
                           int max_height, bool urgent, bool large)
    {
        if (path.empty() || !xemu_hud_uses_d3d12() || max_width <= 0 ||
            max_height <= 0) {
            return nullptr;
        }
        std::string key = ArtKey(path, max_width, max_height);
        ArtTexture &art = m_art[key];
        art.last_used = m_frame;
        art.large = large;
        if (art.state == ArtTexture::State::Ready) {
            return &art;
        }
        if (art.state == ArtTexture::State::Idle &&
            (urgent || m_decoder.Pending() < kMaxPendingDecodes)) {
            art.state = ArtTexture::State::Queued;
            m_decoder.Submit({ key, path, max_width, max_height }, urgent);
        }
        return nullptr;
    }

    // True while requested artwork is still being decoded.
    bool ArtPending(const std::string &path, int max_width, int max_height)
    {
        if (path.empty()) {
            return false;
        }
        auto it = m_art.find(ArtKey(path, max_width, max_height));
        return it != m_art.end() &&
               (it->second.state == ArtTexture::State::Idle ||
                it->second.state == ArtTexture::State::Queued);
    }

    static float ArtFade(const ArtTexture &art, double now)
    {
        return EaseOutCubic((float)((now - art.ready_time) / kArtFade));
    }

    ArtTexture *FindReadyArt(const std::string &key)
    {
        auto it = m_art.find(key);
        if (it == m_art.end() ||
            it->second.state != ArtTexture::State::Ready) {
            return nullptr;
        }
        it->second.last_used = m_frame;
        return &it->second;
    }

    void UploadDecodedArt()
    {
        for (int i = 0; i < kUploadsPerFrame; i++) {
            DecodeResult result;
            if (!m_decoder.TakeResult(&result)) {
                break;
            }
            auto it = m_art.find(result.key);
            if (it == m_art.end()) {
                continue;
            }
            ArtTexture &art = it->second;
            if (result.pixels.empty() ||
                !xemu_hud_upload_d3d12_rgba_texture(
                    result.pixels.data(), result.width, result.height,
                    &art.resource, &art.texture_id)) {
                art.state = ArtTexture::State::Failed;
                continue;
            }
            art.width = result.width;
            art.height = result.height;
            art.ready_time = ImGui::GetTime();
            art.state = ArtTexture::State::Ready;
        }
    }

    void UpdateVideo(double now)
    {
        const LibraryGame *game = Selected();
        std::string want;
        if (game != nullptr && !m_closing &&
            now - m_selection_time >= kVideoDelay) {
            want = game->video_path;
        }
        if (want != m_video.Path()) {
            m_video.SetPath(want);
            m_video_ready = false;
        }
        if (want.empty() || !xemu_hud_uses_d3d12()) {
            return;
        }

        std::vector<uint8_t> pixels;
        int width = 0, height = 0;
        if (!m_video.TakeFrame(&pixels, &width, &height)) {
            return;
        }
        if (m_video_resource != nullptr &&
            (width != m_video_width || height != m_video_height)) {
            ReleaseVideoTexture();
        }
        if (xemu_hud_upload_d3d12_rgba_texture(pixels.data(), width, height,
                                               &m_video_resource,
                                               &m_video_texture)) {
            m_video_width = width;
            m_video_height = height;
            if (!m_video_ready) {
                m_video_ready = true;
                m_video_time = now;
            }
        }
    }

    void EvictArt()
    {
        size_t small_ready = 0;
        for (auto it = m_art.begin(); it != m_art.end();) {
            ArtTexture &art = it->second;
            uint64_t idle = m_frame - art.last_used;
            if (art.state == ArtTexture::State::Ready && art.large &&
                idle > kLargeTextureIdleFrames) {
                xemu_hud_release_d3d12_texture(art.resource);
                it = m_art.erase(it);
                continue;
            }
            if (art.state == ArtTexture::State::Idle && idle > 600) {
                it = m_art.erase(it);
                continue;
            }
            if (art.state == ArtTexture::State::Ready && !art.large) {
                small_ready++;
            }
            ++it;
        }
        if (small_ready <= kMaxSmallTextures) {
            return;
        }

        std::vector<std::pair<uint64_t, std::string>> candidates;
        for (auto &entry : m_art) {
            const ArtTexture &art = entry.second;
            if (art.state == ArtTexture::State::Ready && !art.large &&
                art.last_used + 1 < m_frame) {
                candidates.emplace_back(art.last_used, entry.first);
            }
        }
        std::sort(candidates.begin(), candidates.end());
        for (auto &candidate : candidates) {
            if (small_ready <= kMaxSmallTextures) {
                break;
            }
            auto it = m_art.find(candidate.second);
            xemu_hud_release_d3d12_texture(it->second.resource);
            m_art.erase(it);
            small_ready--;
        }
    }

    void Launch(int index)
    {
        if (index < 0 || index >= (int)m_games.size()) {
            return;
        }
        char *path =
            xemu_game_library_resolve_token(m_games[index].token.c_str());
        if (path == nullptr) {
            xemu_queue_error_message("The selected game is no longer "
                                     "available. Refresh the library and "
                                     "try again.");
            xemu_game_library_refresh();
            return;
        }
        bool loaded = ActionLoadDiscFileChecked(path);
        g_free(path);
        if (!loaded) {
            return;
        }
        // Boot keys belong to the game's shader list too.
        uint32_t title_id = m_games[index].title_id;
        nv2a_shader_cache_set_title(title_id);
        ActionReset();
        if (title_id != 0 && g_config.perf.cache_shaders &&
            g_config.perf.precompile_shaders &&
            g_config.perf.precompile_shaders_wait) {
            m_preparing = true;
            m_prepare_time = ImGui::GetTime();
            m_prepare_name = m_games[index].label;
            m_prepare_status = {};
            nv2a_shader_precompile_set_foreground(true);
            return;
        }
        if (!runstate_is_running()) {
            vm_start();
        }
        BeginClose();
    }

    // Holds the launch until the title's shaders are built or B is
    // pressed; returns false once the game has been started.
    bool UpdatePreparing(double now)
    {
        NV2AShaderPrecompileStatus status;
        nv2a_shader_precompile_status(&status);

        bool waiting = status.state == NV2A_SHADER_PRECOMPILE_COMPILING ||
                       (status.state == NV2A_SHADER_PRECOMPILE_LOADING &&
                        now - m_prepare_time < kPrepareLoadTimeout);
        bool skip = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (!waiting || skip) {
            BeginClose();
            return false;
        }
        m_prepare_status = status;
        return true;
    }

    void DrawPreparing(ImDrawList *dl, ImVec2 display, float alpha)
    {
        const NV2AShaderPrecompileStatus &status = m_prepare_status;
        unsigned int finished = status.done + status.failed;
        float progress = status.total != 0 ?
                             std::min((float)finished / status.total, 1.0f) :
                             0.0f;
        ImFont *big = g_font_mgr.m_menu_font;
        ImFont *small = g_font_mgr.m_menu_font_small;
        float pad = small->FontSize;
        float bar_height = small->FontSize * 0.45f;
        float width = std::min(display.x * 0.6f, big->FontSize * 18.0f);
        float height = pad * 2 + big->FontSize * 1.2f +
                       small->FontSize * 2.9f + bar_height;
        float rounding = small->FontSize * 0.4f;
        ImVec2 a((display.x - width) * 0.5f, (display.y - height) * 0.5f);
        ImVec2 b(a.x + width, a.y + height);
        ImU32 text = IM_COL32(235, 237, 240, (int)(255 * alpha));
        ImU32 detail = IM_COL32(210, 214, 220, (int)(230 * alpha));

        dl->AddRectFilled(ImVec2(0, 0), display,
                          IM_COL32(0, 0, 0, (int)(150 * alpha)));
        dl->AddRectFilled(a, b, IM_COL32(24, 27, 31, (int)(240 * alpha)),
                          rounding);
        dl->AddRect(a, b, IM_COL32(255, 255, 255, (int)(40 * alpha)),
                    rounding);
        dl->PushClipRect(a, b, true);

        float x = a.x + pad;
        float right = b.x - pad;
        float y = a.y + pad;
        dl->AddText(big, big->FontSize, ImVec2(x, y), text,
                    m_prepare_name.c_str());
        y += big->FontSize * 1.2f;

        char count[32];
        snprintf(count, sizeof(count), "%u / %u", finished, status.total);
        ImVec2 count_size =
            small->CalcTextSizeA(small->FontSize, FLT_MAX, 0, count);
        dl->AddText(small, small->FontSize, ImVec2(x, y), detail,
                    "Compiling shaders");
        dl->AddText(small, small->FontSize,
                    ImVec2(right - count_size.x, y), detail, count);
        y += small->FontSize * 1.4f;

        dl->AddRectFilled(ImVec2(x, y), ImVec2(right, y + bar_height),
                          IM_COL32(255, 255, 255, (int)(40 * alpha)),
                          bar_height * 0.5f);
        if (progress > 0.0f) {
            dl->AddRectFilled(ImVec2(x, y),
                              ImVec2(x + (right - x) * progress,
                                     y + bar_height),
                              IM_COL32(255, 255, 255, (int)(220 * alpha)),
                              bar_height * 0.5f);
        }
        y += bar_height + small->FontSize * 0.5f;

        dl->AddText(small, small->FontSize, ImVec2(x, y), text,
                    ICON_BUTTON_B);
        x += small->CalcTextSizeA(small->FontSize, FLT_MAX, 0,
                                  ICON_BUTTON_B).x +
             small->FontSize * 0.35f;
        dl->AddText(small, small->FontSize, ImVec2(x, y), text,
                    "Start now");
        dl->PopClipRect();
    }

    void StartWithoutDisc()
    {
        ActionEjectDisc();
        nv2a_shader_cache_set_title(0);
        ActionReset();
        if (!runstate_is_running()) {
            vm_start();
        }
        BeginClose();
    }

    bool CanReturnToGame() { return !runstate_check(RUN_STATE_PRELAUNCH); }

    void ReturnToGame()
    {
        if (m_was_running && !runstate_is_running()) {
            vm_start();
        }
        BeginClose();
    }

    void OpenSettings(GameLibraryScene &scene)
    {
        g_scene_mgr.PushScene(g_main_menu);
        g_scene_mgr.PushScene(scene);
        BeginClose();
    }

    void HandleInput(GameLibraryScene &scene, int visible,
                     const std::vector<ImVec4> &tile_rects)
    {
        auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key); };
        auto pressed_once = [](ImGuiKey key) {
            return ImGui::IsKeyPressed(key, false);
        };
        int count = (int)m_games.size();

        if (count > 0) {
            int target = m_selected;
            if (pressed(ImGuiKey_GamepadDpadLeft) ||
                pressed(ImGuiKey_GamepadLStickLeft) ||
                pressed(ImGuiKey_LeftArrow)) {
                target = std::max(0, m_selected - 1);
            }
            if (pressed(ImGuiKey_GamepadDpadRight) ||
                pressed(ImGuiKey_GamepadLStickRight) ||
                pressed(ImGuiKey_RightArrow)) {
                target = std::min(count - 1, m_selected + 1);
            }
            int page = std::max(1, visible - 1);
            if (pressed(ImGuiKey_GamepadL1) || pressed(ImGuiKey_PageUp)) {
                target = std::max(0, m_selected - page);
            }
            if (pressed(ImGuiKey_GamepadR1) || pressed(ImGuiKey_PageDown)) {
                target = std::min(count - 1, m_selected + page);
            }
            if (pressed_once(ImGuiKey_Home)) {
                target = 0;
            }
            if (pressed_once(ImGuiKey_End)) {
                target = count - 1;
            }

            ImGuiIO &io = ImGui::GetIO();
            if (io.MouseWheel != 0.0f) {
                target = std::clamp(m_selected + (io.MouseWheel > 0 ? -1 : 1),
                                    0, count - 1);
            }
            if (g_input_mgr.MouseMoved() || ImGui::IsMouseClicked(0)) {
                for (size_t i = 0; i < tile_rects.size(); i++) {
                    const ImVec4 &r = tile_rects[i];
                    if (r.z <= r.x || !ImGui::IsMouseHoveringRect(
                                          ImVec2(r.x, r.y), ImVec2(r.z, r.w),
                                          false)) {
                        continue;
                    }
                    if (ImGui::IsMouseClicked(0) && (int)i == m_selected) {
                        Launch((int)i);
                        return;
                    }
                    target = (int)i;
                    break;
                }
            }
            Select(target);

            if (pressed_once(ImGuiKey_GamepadFaceDown) ||
                pressed_once(ImGuiKey_Enter) ||
                pressed_once(ImGuiKey_KeypadEnter)) {
                Launch(m_selected);
                return;
            }
        }

        if (pressed_once(ImGuiKey_GamepadFaceUp) ||
            pressed_once(ImGuiKey_F3)) {
            StartWithoutDisc();
        } else if (pressed_once(ImGuiKey_GamepadFaceLeft) ||
                   pressed_once(ImGuiKey_F5)) {
            xemu_game_library_refresh();
            xemu_queue_notification("Refreshing game library");
        } else if (pressed_once(ImGuiKey_GamepadStart) ||
                   pressed_once(ImGuiKey_F1)) {
            OpenSettings(scene);
        } else if ((pressed_once(ImGuiKey_GamepadFaceRight) ||
                    pressed_once(ImGuiKey_Escape)) &&
                   CanReturnToGame()) {
            ReturnToGame();
        }
    }

    void DrawImageFill(ImDrawList *dl, ImTextureID texture, int width,
                       int height, ImVec2 a, ImVec2 b, ImU32 tint,
                       float rounding)
    {
        ImVec2 uv0, uv1;
        CropUv((float)width / height, (b.x - a.x) / (b.y - a.y), &uv0, &uv1);
        dl->AddImageRounded(texture, a, b, uv0, uv1, tint, rounding);
    }

    // Fits the image inside [a, b]; align: 0 left/top, 0.5 centre, 1 right.
    ImVec4 DrawImageFit(ImDrawList *dl, const ArtTexture &art, ImVec2 a,
                        ImVec2 b, ImU32 tint, float align_x, float align_y)
    {
        float scale = std::min((b.x - a.x) / art.width,
                               (b.y - a.y) / art.height);
        ImVec2 size(art.width * scale, art.height * scale);
        float x = a.x + (b.x - a.x - size.x) * align_x;
        float y = a.y + (b.y - a.y - size.y) * align_y;
        dl->AddImage((ImTextureID)art.texture_id, ImVec2(x, y),
                     ImVec2(x + size.x, y + size.y), ImVec2(0, 0),
                     ImVec2(1, 1), tint);
        return ImVec4(x, y, x + size.x, y + size.y);
    }

    // Slowly drifting full screen artwork.
    void DrawDriftingImage(ImDrawList *dl, ImTextureID texture, int width,
                           int height, ImVec2 display, float alpha,
                           double now)
    {
        float drift = 0.5f - 0.5f * cosf((float)(now * kTau / 28.0));
        float zoom = 1.06f + 0.06f * drift;
        ImVec2 size(display.x * zoom, display.y * zoom);
        ImVec2 offset(sinf((float)(now * kTau / 41.0)) * display.x * 0.015f,
                      cosf((float)(now * kTau / 53.0)) * display.y * 0.015f);
        ImVec2 a((display.x - size.x) * 0.5f + offset.x,
                 (display.y - size.y) * 0.5f + offset.y);
        ImVec2 uv0, uv1;
        CropUv((float)width / height, size.x / size.y, &uv0, &uv1);
        dl->AddImage(texture, a, a + size, uv0, uv1,
                     IM_COL32(255, 255, 255, (int)(255 * alpha)));
    }

    void DrawBackgroundImage(ImDrawList *dl, const ArtTexture &art,
                             ImVec2 display, float alpha, double now,
                             bool contain)
    {
        if (contain) {
            // Mix images are composites; cropping would cut them off.
            DrawImageFit(dl, art, ImVec2(0, 0), display,
                         IM_COL32(255, 255, 255, (int)(255 * alpha)), 0.5f,
                         0.5f);
        } else {
            DrawDriftingImage(dl, (ImTextureID)art.texture_id, art.width,
                              art.height, display, alpha, now);
        }
    }

    void DrawBackground(ImDrawList *dl, ImVec2 display, float alpha,
                        double now)
    {
        int style = g_config.general.screenscraper.background;
        bool contain = style == ss::kBackgroundMixV1 ||
                       style == ss::kBackgroundMixV2;

        dl->AddRectFilledMultiColor(ImVec2(0, 0), display,
                                    IM_COL32(20, 26, 36, (int)(255 * alpha)),
                                    IM_COL32(20, 26, 36, (int)(255 * alpha)),
                                    IM_COL32(6, 8, 12, (int)(255 * alpha)),
                                    IM_COL32(6, 8, 12, (int)(255 * alpha)));
        if (style == ss::kBackgroundOff) {
            return;
        }

        // The current image stays up while the next decodes; a game without
        // a usable background fades back to the gradient.
        const LibraryGame *game = Selected();
        if (game != nullptr && now - m_selection_time >= kBackgroundDelay) {
            int max_w = (int)display.x, max_h = (int)display.y;
            std::string key;
            if (RequestArt(game->background_path, max_w, max_h, true, true)) {
                key = ArtKey(game->background_path, max_w, max_h);
            }
            if (key != m_background_key &&
                (!key.empty() ||
                 !ArtPending(game->background_path, max_w, max_h))) {
                m_previous_background_key = m_background_key;
                m_background_key = key;
                m_background_time = now;
            }
        }

        float fade = EaseOutCubic((float)((now - m_background_time) /
                                          kBackgroundFade));
        ArtTexture *current = FindReadyArt(m_background_key);
        if (fade < 1.0f) {
            ArtTexture *previous = FindReadyArt(m_previous_background_key);
            if (previous != nullptr) {
                DrawBackgroundImage(dl, *previous, display,
                                    current != nullptr ? alpha :
                                                         alpha * (1.0f - fade),
                                    now, contain);
            }
        } else {
            m_previous_background_key.clear();
        }
        if (current != nullptr) {
            DrawBackgroundImage(dl, *current, display, alpha * fade, now,
                                contain);
        }

        if (m_video_ready && m_video_texture != 0) {
            float video_alpha = EaseOutCubic(
                (float)((now - m_video_time) / kBackgroundFade));
            ImVec2 uv0, uv1;
            CropUv((float)m_video_width / m_video_height,
                   display.x / display.y, &uv0, &uv1);
            dl->AddImage((ImTextureID)m_video_texture, ImVec2(0, 0), display,
                         uv0, uv1,
                         IM_COL32(255, 255, 255,
                                  (int)(255 * alpha * video_alpha)));
        }

        // Keep the details and the row readable over any artwork.
        ImU32 top = IM_COL32(0, 0, 0, (int)(255 * alpha * 0.35f));
        ImU32 bottom = IM_COL32(0, 0, 0, (int)(255 * alpha * 0.88f));
        dl->AddRectFilledMultiColor(ImVec2(0, 0), display, top, top, bottom,
                                    bottom);
        ImU32 side = IM_COL32(0, 0, 0, (int)(255 * alpha * 0.6f));
        ImU32 clear = IM_COL32(0, 0, 0, 0);
        dl->AddRectFilledMultiColor(ImVec2(0, 0),
                                    ImVec2(display.x * 0.65f, display.y),
                                    side, clear, clear, side);
    }

    void DrawPlaceholder(ImDrawList *dl, const LibraryGame &game, ImVec2 a,
                         ImVec2 b, float alpha, float rounding)
    {
        dl->AddRectFilledMultiColor(a, b,
                                    IM_COL32(52, 58, 66, (int)(235 * alpha)),
                                    IM_COL32(52, 58, 66, (int)(235 * alpha)),
                                    IM_COL32(24, 27, 31, (int)(235 * alpha)),
                                    IM_COL32(24, 27, 31, (int)(235 * alpha)));
        dl->AddRect(a, b, IM_COL32(255, 255, 255, (int)(40 * alpha)),
                    rounding);

        ImFont *font = g_font_mgr.m_menu_font_small;
        float pad = std::min(b.x - a.x, b.y - a.y) * 0.1f;
        float wrap = b.x - a.x - pad * 2;
        ImVec2 text = font->CalcTextSizeA(font->FontSize, FLT_MAX, wrap,
                                          game.label.c_str());
        ImVec2 pos(a.x + pad, a.y + (b.y - a.y - text.y) * 0.5f);
        dl->PushClipRect(a, b, true);
        dl->AddText(font, font->FontSize, pos,
                    IM_COL32(230, 232, 235, (int)(255 * alpha)),
                    game.label.c_str(), nullptr, wrap);
        dl->PopClipRect();
    }

    void DrawHints(ImDrawList *dl, ImVec2 pos, float alpha, bool can_return,
                   bool has_games)
    {
        struct Hint {
            const char *glyph;
            bool fa;
            const char *label;
        };
        std::vector<Hint> hints;
        if (has_games) {
            hints.push_back({ ICON_BUTTON_A, false, "Play" });
        }
        hints.push_back({ ICON_BUTTON_Y, false, "Start without disc" });
        hints.push_back({ ICON_BUTTON_X, false, "Refresh" });
        hints.push_back({ ICON_FA_GEARS, true, "Settings" });
        if (can_return) {
            hints.push_back({ ICON_BUTTON_B, false, "Back" });
        }

        ImFont *font = g_font_mgr.m_menu_font_small;
        ImFont *icons = g_font_mgr.m_menu_font;
        float size = font->FontSize;
        ImU32 color = IM_COL32(235, 237, 240, (int)(230 * alpha));
        float x = pos.x;
        for (const Hint &hint : hints) {
            ImFont *glyph_font = hint.fa ? icons : font;
            float glyph_size = hint.fa ? size * 0.8f : size;
            float glyph_y = hint.fa ? pos.y + size * 0.1f : pos.y;
            dl->AddText(glyph_font, glyph_size, ImVec2(x, glyph_y), color,
                        hint.glyph);
            x += glyph_font->CalcTextSizeA(glyph_size, FLT_MAX, 0,
                                           hint.glyph).x +
                 size * 0.35f;
            dl->AddText(font, size, ImVec2(x, pos.y), color, hint.label);
            x += font->CalcTextSizeA(size, FLT_MAX, 0, hint.label).x +
                 size * 1.4f;
        }
    }

    void DrawClock(ImDrawList *dl, ImVec2 display, float margin_x, float top,
                   float alpha)
    {
        GDateTime *time = g_date_time_new_now_local();
        if (time == nullptr) {
            return;
        }
        gchar *clock = g_date_time_format(time, "%H:%M");
        g_date_time_unref(time);
        if (clock == nullptr) {
            return;
        }
        ImFont *font = g_font_mgr.m_menu_font_small;
        ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, clock);
        dl->AddText(font, font->FontSize,
                    ImVec2(display.x - margin_x - size.x, top),
                    IM_COL32(220, 224, 228, (int)(200 * alpha)), clock);
        g_free(clock);
    }

    // Clear logo (or the title as text), metadata, rating and synopsis on the
    // left; the large box art on the right.
    void DrawDetails(ImDrawList *dl, ImVec2 display, float margin_x,
                     float top, float bottom, float alpha, double now)
    {
        const LibraryGame *game = Selected();
        if (game == nullptr) {
            return;
        }

        float art_left = display.x * 0.62f;
        int large_w = (int)(display.x * 0.34f);
        int large_h = (int)(bottom - top);
        ArtTexture *large = RequestArt(game->large_art_path, large_w, large_h,
                                       true, true);
        if (large != nullptr) {
            DrawImageFit(dl, *large, ImVec2(art_left, top),
                         ImVec2(display.x - margin_x, bottom),
                         IM_COL32(255, 255, 255,
                                  (int)(255 * alpha * ArtFade(*large, now))),
                         1.0f, 1.0f);
        }

        // The text keeps its width while the large art decodes.
        bool has_large = large != nullptr ||
                         ArtPending(game->large_art_path, large_w, large_h);
        float column = (has_large ? art_left - display.x * 0.03f :
                                    display.x * 0.72f) -
                       margin_x;
        float y = top;
        float logo_h = (bottom - top) * 0.36f;
        int logo_w = (int)(column * 0.85f);
        ArtTexture *logo =
            RequestArt(game->logo_path, logo_w, (int)logo_h, true, false);
        if (logo != nullptr) {
            ImVec4 r = DrawImageFit(
                dl, *logo, ImVec2(margin_x, y),
                ImVec2(margin_x + column * 0.85f, y + logo_h),
                IM_COL32(255, 255, 255,
                         (int)(255 * alpha * ArtFade(*logo, now))),
                0.0f, 1.0f);
            y = r.w + display.y * 0.022f;
        } else if (ArtPending(game->logo_path, logo_w, (int)logo_h)) {
            // Hold the logo's space rather than flash the title text.
            y = top + logo_h + display.y * 0.022f;
        } else {
            ImFont *font = g_font_mgr.m_menu_font;
            float size = font->FontSize * 1.3f;
            ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, column,
                                              game->label.c_str());
            y = std::max(top, top + logo_h - text.y);
            dl->AddText(font, size, ImVec2(margin_x + 2, y + 2),
                        IM_COL32(0, 0, 0, (int)(160 * alpha)),
                        game->label.c_str(), nullptr, column);
            dl->AddText(font, size, ImVec2(margin_x, y),
                        IM_COL32(255, 255, 255, (int)(255 * alpha)),
                        game->label.c_str(), nullptr, column);
            y += text.y + display.y * 0.018f;
        }

        ImFont *small = g_font_mgr.m_menu_font_small;
        float line = small->FontSize;
        std::vector<std::string> fields;
        if (!game->release_date.empty()) {
            fields.push_back(FormatReleaseDate(game->release_date));
        }
        if (!game->developer.empty()) {
            fields.push_back(game->developer);
        }
        if (!game->publisher.empty() && game->publisher != game->developer) {
            fields.push_back(game->publisher);
        }
        if (!game->genre.empty()) {
            fields.push_back(game->genre);
        }
        if (!game->players.empty()) {
            fields.push_back(FormatPlayers(game->players));
        }
        std::string meta;
        for (const std::string &field : fields) {
            if (!meta.empty()) {
                meta += "   \xC2\xB7   ";
            }
            meta += field;
        }
        if (!meta.empty()) {
            y += DrawWrappedLines(dl, small, line, ImVec2(margin_x, y), column,
                                  2, IM_COL32(225, 228, 232,
                                              (int)(235 * alpha)),
                                  meta);
            y += line * 0.25f;
        }

        float stars = RatingStars(game->rating);
        if (stars >= 0.0f) {
            DrawStars(dl, ImVec2(margin_x, y), line * 0.85f, stars, alpha);
            y += line * 1.3f;
        }

        if (!game->synopsis.empty() && y + line < bottom) {
            int max_lines = std::clamp((int)((bottom - y) / (line * 1.15f)),
                                       1, 6);
            DrawWrappedLines(dl, small, line, ImVec2(margin_x, y + line * 0.2f),
                             column, max_lines,
                             IM_COL32(205, 209, 214, (int)(225 * alpha)),
                             game->synopsis);
        }
    }

    void DrawEmpty(ImDrawList *dl, ImVec2 display, float alpha, double now)
    {
        bool searching = now - m_show_time < 3.0;
        const char *title = searching ? "Looking for games\xE2\x80\xA6" :
                                        "No games found";
        const char *detail =
            searching ? "" :
                        "Copy disc images to the games folder, or choose "
                        "another folder in Settings.";
        ImFont *big = g_font_mgr.m_menu_font;
        ImFont *small = g_font_mgr.m_menu_font_small;
        ImVec2 title_size = big->CalcTextSizeA(big->FontSize, FLT_MAX, 0,
                                               title);
        float y = display.y * 0.45f;
        dl->AddText(big, big->FontSize,
                    ImVec2((display.x - title_size.x) * 0.5f, y),
                    IM_COL32(255, 255, 255, (int)(255 * alpha)), title);
        if (detail[0]) {
            ImVec2 detail_size = small->CalcTextSizeA(small->FontSize,
                                                      FLT_MAX, 0, detail);
            dl->AddText(small, small->FontSize,
                        ImVec2((display.x - detail_size.x) * 0.5f,
                               y + title_size.y + small->FontSize * 0.6f),
                        IM_COL32(210, 214, 220, (int)(220 * alpha)), detail);
        }
    }

    void DrawRow(ImDrawList *dl, ImVec2 display, float margin_x,
                 float row_top, float tile_w, float tile_h, float gap,
                 float alpha, double now, std::vector<ImVec4> *tile_rects)
    {
        int style = g_config.general.screenscraper.box_art_style;
        bool cutout = IsCutoutStyle(style);
        int count = (int)m_games.size();
        float pitch = tile_w + gap;
        float grow = EaseOutCubic((float)((now - m_selection_time) /
                                          kSelectionGrow));
        float rounding = std::min(tile_w, tile_h) * 0.04f;
        int art_w = (int)(tile_w * 1.12f);
        int art_h = (int)(tile_h * 1.12f);

        int first = std::max(0, (int)floorf(m_scroll) - 1);
        int last = std::min(count - 1,
                            (int)ceilf(m_scroll) +
                                (int)(display.x / pitch) + 1);

        for (int pass = 0; pass < 2; pass++) {
            for (int index = first; index <= last; index++) {
                bool selected = index == m_selected;
                // Draw the selected tile last so it overlaps its neighbours.
                if (selected != (pass == 1)) {
                    continue;
                }
                const LibraryGame &game = m_games[index];
                float x = margin_x + (index - m_scroll) * pitch;
                ImVec2 a(x, row_top), b(x + tile_w, row_top + tile_h);
                float edge_alpha = 1.0f;
                if (b.x > display.x - margin_x * 0.5f) {
                    edge_alpha = std::clamp(
                        1.0f - (b.x - (display.x - margin_x * 0.5f)) / pitch,
                        0.0f, 1.0f);
                } else if (a.x < margin_x * 0.5f) {
                    edge_alpha = std::clamp(
                        1.0f - (margin_x * 0.5f - a.x) / pitch, 0.0f, 1.0f);
                }
                if (edge_alpha <= 0.0f) {
                    continue;
                }
                if (edge_alpha >= 0.5f) {
                    (*tile_rects)[index] = ImVec4(a.x, a.y, b.x, b.y);
                }

                float tile_alpha = alpha * edge_alpha;
                if (selected) {
                    float scale = 1.0f + 0.1f * grow;
                    ImVec2 center((a.x + b.x) * 0.5f, b.y);
                    ImVec2 half((b.x - a.x) * 0.5f * scale,
                                (b.y - a.y) * scale);
                    a = ImVec2(center.x - half.x, center.y - half.y);
                    b = ImVec2(center.x + half.x, center.y);
                }

                ArtTexture *art = RequestArt(game.cover_path, art_w, art_h,
                                             selected, false);
                ImU32 tint = selected ? IM_COL32(255, 255, 255, 255) :
                                        IM_COL32(190, 190, 190, 255);
                tint = WithAlpha(tint, tile_alpha);

                if (art != nullptr && cutout) {
                    if (selected) {
                        dl->AddRectFilled(
                            a, b,
                            IM_COL32(255, 255, 255,
                                     (int)(28 * tile_alpha * grow)),
                            rounding);
                    }
                    DrawImageFit(dl, *art, a, b, tint, 0.5f, 1.0f);
                } else if (art != nullptr) {
                    dl->AddRectFilled(a + ImVec2(0, tile_h * 0.025f),
                                      b + ImVec2(0, tile_h * 0.04f),
                                      IM_COL32(0, 0, 0,
                                               (int)(110 * tile_alpha)),
                                      rounding);
                    float aspect = (float)art->width / art->height;
                    float tile_aspect = (b.x - a.x) / (b.y - a.y);
                    if (fabsf(aspect / tile_aspect - 1.0f) < 0.25f) {
                        DrawImageFill(dl, (ImTextureID)art->texture_id,
                                      art->width, art->height, a, b, tint,
                                      rounding);
                    } else {
                        dl->AddRectFilled(a, b,
                                          IM_COL32(20, 22, 26,
                                                   (int)(220 * tile_alpha)),
                                          rounding);
                        DrawImageFit(dl, *art, a, b, tint, 0.5f, 0.5f);
                    }
                } else {
                    DrawPlaceholder(dl, game, a, b, tile_alpha, rounding);
                }

                if (selected) {
                    float pulse = 0.75f + 0.25f * sinf((float)now * 3.0f);
                    for (int ring = 1; ring <= 3; ring++) {
                        float spread = ring * tile_h * 0.008f;
                        dl->AddRect(a - ImVec2(spread, spread),
                                    b + ImVec2(spread, spread),
                                    IM_COL32(255, 255, 255,
                                             (int)(70 * tile_alpha * grow *
                                                   pulse / ring)),
                                    rounding + spread, 0, 2.0f);
                    }
                    if (!cutout || art == nullptr) {
                        dl->AddRect(a, b,
                                    IM_COL32(255, 255, 255,
                                             (int)(255 * tile_alpha)),
                                    rounding, 0,
                                    std::max(2.0f, tile_h * 0.008f));
                    }
                }
            }
        }
    }

    bool Draw(GameLibraryScene &scene)
    {
        m_fade.Step();
        if (m_closing && m_fade.IsComplete()) {
            Teardown();
            return false;
        }

        // A launch from elsewhere (network frontend, popup action) started
        // the machine; get out of the way.
        if (!m_closing && runstate_is_running()) {
            BeginClose();
        }

        m_frame++;
        SyncGames();
        UploadDecodedArt();

        ImGuiIO &io = ImGui::GetIO();
        ImVec2 display = io.DisplaySize;
        double now = ImGui::GetTime();
        float alpha = m_fade.GetSinInterpolatedValue();
        float dt = std::max(io.DeltaTime, 0.0f);

        UpdateVideo(now);
        if (!m_closing) {
            SyncRemoteSelection(now);
        }

        ImVec4 extents = g_viewport_mgr.GetExtents();
        float margin_x = std::max(extents.x, display.x * 0.05f);
        float top = std::max(extents.y, display.y * 0.055f);
        float bottom_margin = std::max(extents.w, display.y * 0.05f);
        float hint_height = g_font_mgr.m_menu_font_small->FontSize;

        int style = g_config.general.screenscraper.box_art_style;
        float tile_h = std::round(display.y * 0.235f);
        float tile_w = std::round(tile_h * TileAspect(style));
        float gap = std::round(display.y * 0.026f);
        float row_bottom = display.y - bottom_margin - hint_height * 2.0f;
        float row_top = row_bottom - tile_h;
        int count = (int)m_games.size();
        int visible = std::max(
            1, (int)((display.x - 2 * margin_x + gap) / (tile_w + gap)));

        // Keep the selected game in the second slot where possible.
        m_first = std::clamp(m_selected - 1, 0, std::max(0, count - visible));
        m_scroll += (m_first - m_scroll) * (1.0f - expf(-dt * 14.0f));
        if (fabsf(m_scroll - m_first) < 0.001f) {
            m_scroll = (float)m_first;
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(display);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::Begin("###GameLibrary", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList *dl = ImGui::GetWindowDrawList();

        DrawBackground(dl, display, alpha, now);
        DrawClock(dl, display, margin_x, top, alpha);

        std::vector<ImVec4> tile_rects(count, ImVec4(0, 0, 0, 0));
        if (count == 0) {
            DrawEmpty(dl, display, alpha, now);
        } else {
            DrawDetails(dl, display, margin_x,
                        top + hint_height + display.y * 0.03f,
                        row_top - tile_h * 0.16f, alpha, now);
            DrawRow(dl, display, margin_x, row_top, tile_w, tile_h, gap, alpha,
                    now, &tile_rects);
        }

        DrawHints(dl,
                  ImVec2(margin_x, display.y - bottom_margin - hint_height),
                  alpha, CanReturnToGame(), count > 0);

        bool preparing = m_preparing && !m_closing && UpdatePreparing(now);
        if (preparing &&
            m_prepare_status.state == NV2A_SHADER_PRECOMPILE_COMPILING &&
            now - m_prepare_time >= kPrepareShowDelay) {
            DrawPreparing(dl, display, alpha);
        }

        ImGui::End();
        ImGui::PopStyleVar(2);

        if (!m_closing && !preparing) {
            HandleInput(scene, visible, tile_rects);
        }
        EvictArt();
        return true;
    }
};

GameLibraryScene g_game_library;

GameLibraryScene::GameLibraryScene() : m_view(new GameLibraryView) {}

GameLibraryScene::~GameLibraryScene() = default;

void GameLibraryScene::Show()
{
    m_view->Show();
}

void GameLibraryScene::Hide()
{
    m_view->BeginClose();
}

bool GameLibraryScene::IsAnimating()
{
    return m_view->m_fade.IsAnimating();
}

bool GameLibraryScene::Draw()
{
    return m_view->Draw(*this);
}

bool GameLibraryScene::IsActive()
{
    return m_view->m_active;
}
