// screen_recorder_plugin.cpp  (Windows)
// Screen capture : DXGI Desktop Duplication → region crop
// Audio capture  : WASAPI microphone (shared mode)
// Encoding/mux   : Media Foundation Sink Writer → H.264 + AAC → MP4

#include "screen_recorder_plugin.h"

#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <flutter/method_result_functions.h>
#include <flutter/plugin_registrar.h>
#include <flutter/standard_method_codec.h>

// WIN32_LEAN_AND_MEAN and NOMINMAX are already set by CMakeLists.txt
#include <windows.h>
#include <wrl/client.h>

// D3D / DXGI
#include <d3d11.h>
#include <dxgi1_2.h>

// Media Foundation
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

// WASAPI / MMDevice
#include <mmdeviceapi.h>
#include <audioclient.h>

// Pragma-linking (MSVC only; avoids changing CMakeLists for extra libs)
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// ─── Constants ────────────────────────────────────────────────────────────────
static const char*    kChannelName = "com.screenrecorder/recorder";
static const int      kFPS         = 30;
static const LONGLONG kFrameDur    = 10000000LL / kFPS;  // 100-nanosecond units

// ─── State ────────────────────────────────────────────────────────────────────
struct RecorderState {
    // Recording configuration
    std::wstring output_path;
    int cap_x = 0, cap_y = 0;   // physical screen origin of the region
    int cap_w = 0, cap_h = 0;   // physical size of the region
    int mon_x = 0, mon_y = 0;   // top-left of the selected monitor in virtual desktop

    // D3D11 / DXGI Desktop Duplication
    ComPtr<ID3D11Device>           d3d_device;
    ComPtr<ID3D11DeviceContext>    d3d_ctx;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D>        staging;    // pre-allocated CPU-readable staging texture

    // Media Foundation
    ComPtr<IMFSinkWriter> sink_writer;
    DWORD vid_idx = 0;
    DWORD aud_idx = 0;
    LONGLONG vid_pts  = 0;   // running presentation timestamp for video (100ns)
    LONGLONG aud_pts  = 0;   // running presentation timestamp for audio (100ns)
    std::mutex mf_lock;       // serialise WriteSample calls across threads

    // WASAPI microphone
    ComPtr<IMMDevice>            mic_device;
    ComPtr<IAudioClient>         audio_client;
    ComPtr<IAudioCaptureClient>  capture_client;
    WAVEFORMATEX*                mix_fmt       = nullptr; // CoTaskMemFree on cleanup
    bool                         has_audio     = false;
    bool                         aud_is_float  = false;   // true = 32-bit float samples
    int                          aud_channels  = 0;
    int                          aud_sample_rate = 0;
    int                          aud_bits      = 0;       // bits per sample in mix_fmt

    // GDI fallback capture (used when DXGI Desktop Duplication is unavailable)
    bool    use_gdi       = false;
    HDC     gdi_screen_dc = nullptr;
    HDC     gdi_mem_dc    = nullptr;
    HBITMAP gdi_bitmap    = nullptr;

    // Thread control
    std::atomic<bool> stop{ false };
    std::thread vid_thread;
    std::thread aud_thread;
};

// One live recording at a time
static std::unique_ptr<RecorderState> g_state;
// HWND of the Flutter top-level window (set during plugin registration)
static HWND g_flutter_hwnd = nullptr;

// Fullscreen state for region selection mode
static WINDOWPLACEMENT g_prev_placement = { sizeof(WINDOWPLACEMENT) };
static LONG g_prev_style = 0;
static LONG g_prev_exstyle = 0;

// ─── String helpers ───────────────────────────────────────────────────────────
static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], n);
    return ws;
}

static std::string from_wide(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// ─── DXGI / D3D11 initialisation ─────────────────────────────────────────────
// Helper: attempt DuplicateOutput with a D3D device created on |adapter|.
// Returns S_OK on success and populates s->d3d_device, d3d_ctx, duplication.
static HRESULT try_duplicate(RecorderState* s,
                             IDXGIAdapter* adapter,
                             IDXGIOutput*  output) {
    s->d3d_device.Reset();
    s->d3d_ctx.Reset();
    s->duplication.Reset();

    D3D_FEATURE_LEVEL fl;
    // When pAdapter is non-null, DriverType MUST be D3D_DRIVER_TYPE_UNKNOWN.
    HRESULT hr = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &s->d3d_device, &fl, &s->d3d_ctx);
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIOutput1> out1;
    hr = output->QueryInterface(IID_PPV_ARGS(&out1));
    if (FAILED(hr)) return hr;

    return out1->DuplicateOutput(s->d3d_device.Get(), &s->duplication);
}

static HRESULT init_dxgi(RecorderState* s) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;

    // Collect every output that contains our capture origin, together with its
    // owning adapter.  On hybrid-GPU laptops the output list is often:
    //   Adapter 0 (dGPU): no outputs
    //   Adapter 1 (iGPU): Output 0 (the display)
    // The D3D device MUST be created on the adapter that owns the output.
    struct Candidate {
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIOutput>  output;
        int mon_x, mon_y;
    };
    std::vector<Candidate> exact;   // output contains cap point
    std::vector<Candidate> fallback; // any output (last resort)

    for (UINT ai = 0; ; ++ai) {
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapters(ai, &adapter))) break;

        for (UINT oi = 0; ; ++oi) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(oi, &output))) break;

            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            RECT& r = desc.DesktopCoordinates;

            Candidate c{ adapter, output, static_cast<int>(r.left),
                                          static_cast<int>(r.top) };

            if (s->cap_x >= r.left && s->cap_x < r.right &&
                s->cap_y >= r.top  && s->cap_y < r.bottom) {
                exact.push_back(std::move(c));
            } else {
                fallback.push_back(std::move(c));
            }
        }
    }

    // Append fallbacks so we try them if exact matches all fail
    for (auto& c : fallback) exact.push_back(std::move(c));

    HRESULT last_hr = DXGI_ERROR_NOT_FOUND;
    for (auto& c : exact) {
        hr = try_duplicate(s, c.adapter.Get(), c.output.Get());
        if (SUCCEEDED(hr)) {
            s->mon_x = c.mon_x;
            s->mon_y = c.mon_y;

            // Pre-allocate CPU-readable staging texture for the capture region
            D3D11_TEXTURE2D_DESC td = {};
            td.Width            = static_cast<UINT>(s->cap_w);
            td.Height           = static_cast<UINT>(s->cap_h);
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_STAGING;
            td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
            return s->d3d_device->CreateTexture2D(&td, nullptr, &s->staging);
        }
        last_hr = hr;
    }

    // Also try the default hardware device as a last resort
    {
        s->d3d_device.Reset();
        s->d3d_ctx.Reset();
        s->duplication.Reset();

        D3D_FEATURE_LEVEL fl;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &s->d3d_device, &fl, &s->d3d_ctx);
        if (SUCCEEDED(hr) && !exact.empty()) {
            ComPtr<IDXGIOutput1> out1;
            if (SUCCEEDED(exact[0].output.As(&out1))) {
                hr = out1->DuplicateOutput(s->d3d_device.Get(),
                                           &s->duplication);
                if (SUCCEEDED(hr)) {
                    s->mon_x = exact[0].mon_x;
                    s->mon_y = exact[0].mon_y;

                    D3D11_TEXTURE2D_DESC td = {};
                    td.Width            = static_cast<UINT>(s->cap_w);
                    td.Height           = static_cast<UINT>(s->cap_h);
                    td.MipLevels        = 1;
                    td.ArraySize        = 1;
                    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
                    td.SampleDesc.Count = 1;
                    td.Usage            = D3D11_USAGE_STAGING;
                    td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
                    return s->d3d_device->CreateTexture2D(
                        &td, nullptr, &s->staging);
                }
                last_hr = hr;
            }
        }
    }

    return last_hr;
}

// ─── GDI fallback capture init ────────────────────────────────────────────────
// Used when DXGI Desktop Duplication is unavailable (driver / GPU limitation).
// BitBlt from the screen DC is universally compatible on all Windows versions.
static HRESULT init_gdi_capture(RecorderState* s) {
    s->gdi_screen_dc = GetDC(nullptr);
    if (!s->gdi_screen_dc) return E_FAIL;

    s->gdi_mem_dc = CreateCompatibleDC(s->gdi_screen_dc);
    if (!s->gdi_mem_dc) {
        ReleaseDC(nullptr, s->gdi_screen_dc);
        s->gdi_screen_dc = nullptr;
        return E_FAIL;
    }

    s->gdi_bitmap = CreateCompatibleBitmap(s->gdi_screen_dc, s->cap_w, s->cap_h);
    if (!s->gdi_bitmap) {
        DeleteDC(s->gdi_mem_dc);      s->gdi_mem_dc = nullptr;
        ReleaseDC(nullptr, s->gdi_screen_dc); s->gdi_screen_dc = nullptr;
        return E_FAIL;
    }

    s->use_gdi = true;
    return S_OK;
}

// ─── Top-level capture init: try DXGI, fall back to GDI ──────────────────────
static HRESULT init_capture(RecorderState* s) {
    HRESULT hr = init_dxgi(s);
    if (SUCCEEDED(hr)) return hr;

    // DXGI Desktop Duplication failed — fall back to GDI BitBlt capture
    s->d3d_device.Reset();
    s->d3d_ctx.Reset();
    s->duplication.Reset();
    s->staging.Reset();

    return init_gdi_capture(s);
}

// ─── WASAPI microphone initialisation ────────────────────────────────────────
static HRESULT init_wasapi(RecorderState* s) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;

    hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &s->mic_device);
    if (FAILED(hr)) return hr;

    hr = s->mic_device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(s->audio_client.GetAddressOf()));
    if (FAILED(hr)) return hr;

    hr = s->audio_client->GetMixFormat(&s->mix_fmt);
    if (FAILED(hr)) return hr;

    // Record the audio format details we need for conversion later
    s->aud_channels    = s->mix_fmt->nChannels;
    s->aud_sample_rate = static_cast<int>(s->mix_fmt->nSamplesPerSec);
    s->aud_bits        = s->mix_fmt->wBitsPerSample;

    if (s->mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        s->aud_is_float = true;
    } else if (s->mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(s->mix_fmt);
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00aa00389b71}
        static const GUID kSubtypeFloat =
            { 0x00000003, 0x0000, 0x0010,
              { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
        s->aud_is_float = (IsEqualGUID(ext->SubFormat, kSubtypeFloat) != FALSE);
        s->aud_bits     = s->mix_fmt->wBitsPerSample;
    }

    // Initialise in shared mode with a 100 ms buffer
    hr = s->audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 0,
        10000000LL, 0, s->mix_fmt, nullptr);
    if (FAILED(hr)) return hr;

    return s->audio_client->GetService(IID_PPV_ARGS(&s->capture_client));
}

// ─── Media Foundation Sink Writer initialisation ─────────────────────────────
// Tries hardware H.264 encoding first; if it fails (buggy driver crash guard),
// automatically retries with software (MS encoder) only.
static HRESULT init_mf_writer_with_attrs(RecorderState* s, bool allow_hw) {
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 2);
    // Only enable hardware transforms if safe to do so.
    // Some Intel iGPU drivers (mfx_mft_h264ve_64.dll) crash with 0xc0000005.
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                     allow_hw ? TRUE : FALSE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    HRESULT hr = MFCreateSinkWriterFromURL(
        s->output_path.c_str(), nullptr, attrs.Get(), &s->sink_writer);
    if (FAILED(hr)) return hr;

    // ── Video stream ──────────────────────────────────────────────────────────
    ComPtr<IMFMediaType> vid_out;
    MFCreateMediaType(&vid_out);
    vid_out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vid_out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    vid_out->SetUINT32(MF_MT_AVG_BITRATE, 4000000);
    vid_out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(vid_out.Get(), MF_MT_FRAME_SIZE,
                       static_cast<UINT32>(s->cap_w),
                       static_cast<UINT32>(s->cap_h));
    MFSetAttributeRatio(vid_out.Get(), MF_MT_FRAME_RATE, kFPS, 1);
    MFSetAttributeRatio(vid_out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = s->sink_writer->AddStream(vid_out.Get(), &s->vid_idx);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> vid_in;
    MFCreateMediaType(&vid_in);
    vid_in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vid_in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    vid_in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    vid_in->SetUINT32(MF_MT_DEFAULT_STRIDE,
                      static_cast<UINT32>(s->cap_w * 4));
    MFSetAttributeSize(vid_in.Get(), MF_MT_FRAME_SIZE,
                       static_cast<UINT32>(s->cap_w),
                       static_cast<UINT32>(s->cap_h));
    MFSetAttributeRatio(vid_in.Get(), MF_MT_FRAME_RATE, kFPS, 1);
    MFSetAttributeRatio(vid_in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = s->sink_writer->SetInputMediaType(s->vid_idx, vid_in.Get(), nullptr);
    if (FAILED(hr)) return hr;

    // ── Audio stream (optional) ───────────────────────────────────────────────
    if (s->has_audio) {
        UINT32 sr = static_cast<UINT32>(s->aud_sample_rate);
        UINT32 ch = static_cast<UINT32>(s->aud_channels);

        ComPtr<IMFMediaType> aud_out;
        MFCreateMediaType(&aud_out);
        aud_out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aud_out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aud_out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aud_out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sr);
        aud_out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        aud_out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);

        HRESULT ahr = s->sink_writer->AddStream(aud_out.Get(), &s->aud_idx);
        if (SUCCEEDED(ahr)) {
            ComPtr<IMFMediaType> aud_in;
            MFCreateMediaType(&aud_in);
            aud_in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aud_in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            aud_in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            aud_in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sr);
            aud_in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
            aud_in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * 2);
            aud_in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sr * ch * 2);
            ahr = s->sink_writer->SetInputMediaType(
                s->aud_idx, aud_in.Get(), nullptr);
        }
        if (FAILED(ahr)) s->has_audio = false;
    }

    return s->sink_writer->BeginWriting();
}

static HRESULT init_mf_writer(RecorderState* s) {
    // First try with hardware acceleration (faster encoding).
    HRESULT hr = init_mf_writer_with_attrs(s, /*allow_hw=*/true);
    if (SUCCEEDED(hr)) return hr;

    // Hardware encoder failed (e.g. buggy Intel mfx_mft_h264ve_64.dll driver).
    // Reset sink writer and retry with software-only encoding.
    s->sink_writer.Reset();
    s->vid_idx = 0;
    s->aud_idx = 0;
    return init_mf_writer_with_attrs(s, /*allow_hw=*/false);
}

// Composite the DXGI cursor onto the BGRA frame buffer.
// Handles both monochrome (AND+XOR mask) and colour cursors.
static void draw_cursor_dxgi(RecorderState* s,
                              BYTE* frame,          // BGRA, cap_w * cap_h * 4
                              const DXGI_OUTDUPL_FRAME_INFO& fi) {
    if (fi.PointerPosition.Visible == FALSE) return;
    if (fi.PointerShapeBufferSize == 0) return;

    // Fetch cursor shape
    UINT shape_size = fi.PointerShapeBufferSize;
    std::vector<BYTE> shape_buf(shape_size);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info = {};
    UINT required = 0;
    HRESULT hr = s->duplication->GetFramePointerShape(
        shape_size, shape_buf.data(), &required, &shape_info);
    if (FAILED(hr)) return;

    // Cursor position on screen (physical pixels, virtual desktop coords)
    int cx = fi.PointerPosition.Position.x - shape_info.HotSpot.x - s->cap_x;
    int cy = fi.PointerPosition.Position.y - shape_info.HotSpot.y - s->cap_y;
    int cw = static_cast<int>(shape_info.Width);
    int ch = static_cast<int>(shape_info.Height);

    if (shape_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        // 32-bit BGRA cursor — straight alpha blend onto frame
        for (int row = 0; row < ch; ++row) {
            int fy = cy + row;
            if (fy < 0 || fy >= s->cap_h) continue;
            for (int col = 0; col < cw; ++col) {
                int fx = cx + col;
                if (fx < 0 || fx >= s->cap_w) continue;
                const BYTE* src = shape_buf.data() +
                                  row * shape_info.Pitch + col * 4;
                BYTE* dst = frame + (fy * s->cap_w + fx) * 4;
                BYTE a = src[3];
                if (a == 0) continue;
                dst[0] = static_cast<BYTE>((src[0] * a + dst[0] * (255 - a)) / 255); // B
                dst[1] = static_cast<BYTE>((src[1] * a + dst[1] * (255 - a)) / 255); // G
                dst[2] = static_cast<BYTE>((src[2] * a + dst[2] * (255 - a)) / 255); // R
            }
        }
    } else if (shape_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // 1-bit AND+XOR mask — height is doubled (top half = AND, bottom = XOR)
        ch /= 2;
        int pitch = shape_info.Pitch;
        for (int row = 0; row < ch; ++row) {
            int fy = cy + row;
            if (fy < 0 || fy >= s->cap_h) continue;
            for (int col = 0; col < cw; ++col) {
                int fx = cx + col;
                if (fx < 0 || fx >= s->cap_w) continue;
                int byte_idx = row * pitch + col / 8;
                int bit      = 7 - (col % 8);
                bool and_bit = (shape_buf[byte_idx]        >> bit) & 1;
                bool xor_bit = (shape_buf[byte_idx + ch * pitch] >> bit) & 1;
                BYTE* dst = frame + (fy * s->cap_w + fx) * 4;
                if (!and_bit && !xor_bit)  { dst[0]=dst[1]=dst[2]=0; }        // black
                else if (!and_bit && xor_bit)  { dst[0]=dst[1]=dst[2]=0xFF; } // white
                else if (and_bit && xor_bit)   { dst[0]^=0xFF; dst[1]^=0xFF; dst[2]^=0xFF; } // invert
                // and=1 xor=0 → transparent, leave dst untouched
            }
        }
    } else {
        // MASKED_COLOR: top half is AND mask (1-bit), bottom half is 32-bit XOR color
        ch /= 2;
        int and_pitch = shape_info.Pitch;
        int xor_offset = ch * and_pitch;
        int xor_pitch  = cw * 4;
        for (int row = 0; row < ch; ++row) {
            int fy = cy + row;
            if (fy < 0 || fy >= s->cap_h) continue;
            for (int col = 0; col < cw; ++col) {
                int fx = cx + col;
                if (fx < 0 || fx >= s->cap_w) continue;
                int bit_idx = row * and_pitch + col / 8;
                int bit     = 7 - (col % 8);
                bool and_bit = (shape_buf[bit_idx] >> bit) & 1;
                const BYTE* xor_px = shape_buf.data() + xor_offset +
                                     row * xor_pitch + col * 4;
                BYTE* dst = frame + (fy * s->cap_w + fx) * 4;
                if (and_bit) {
                    dst[0] ^= xor_px[0];
                    dst[1] ^= xor_px[1];
                    dst[2] ^= xor_px[2];
                } else {
                    dst[0] = xor_px[0];
                    dst[1] = xor_px[1];
                    dst[2] = xor_px[2];
                }
            }
        }
    }
}
// ─── Video capture thread ─────────────────────────────────────────────────────
static void video_thread_func(RecorderState* s) {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const DWORD buf_bytes = static_cast<DWORD>(s->cap_w * s->cap_h * 4);
    std::vector<BYTE> last_frame(buf_bytes, 0);
    bool have_frame = false;

    const auto frame_period = std::chrono::nanoseconds(1000000000LL / kFPS);
    auto next_write = std::chrono::steady_clock::now();

    while (!s->stop.load()) {
        // Sleep until the next 30-fps slot
        {
            auto now  = std::chrono::steady_clock::now();
            auto wait = next_write - now;
            if (wait > std::chrono::milliseconds(1)) {
                std::this_thread::sleep_for(
                    wait - std::chrono::milliseconds(1));  // wake slightly early
            }
            // Spin for the remaining sub-millisecond
            while (std::chrono::steady_clock::now() < next_write && !s->stop.load()) {}
        }
        if (s->stop.load()) break;
        next_write += frame_period;

        // ── Acquire a frame ────────────────────────────────────────────────
if (s->use_gdi) {
            HGDIOBJ old = SelectObject(s->gdi_mem_dc, s->gdi_bitmap);
            // CAPTUREBLT alone doesn't capture cursor; draw it manually below
            BitBlt(s->gdi_mem_dc, 0, 0, s->cap_w, s->cap_h,
                   s->gdi_screen_dc, s->cap_x, s->cap_y, SRCCOPY | CAPTUREBLT);

            // Draw cursor onto the memory DC at its current screen position
            CURSORINFO ci = { sizeof(CURSORINFO) };
            if (GetCursorInfo(&ci) && ci.flags == CURSOR_SHOWING) {
                int cx = ci.ptScreenPos.x - s->cap_x;
                int cy = ci.ptScreenPos.y - s->cap_y;
                DrawIconEx(s->gdi_mem_dc, cx, cy, ci.hCursor,
                           0, 0, 0, nullptr, DI_NORMAL);
            }

            SelectObject(s->gdi_mem_dc, old);

            BITMAPINFOHEADER bi = {};
            bi.biSize        = sizeof(bi);
            bi.biWidth       = s->cap_w;
            bi.biHeight      = -(s->cap_h);
            bi.biPlanes      = 1;
            bi.biBitCount    = 32;
            bi.biCompression = BI_RGB;

            GetDIBits(s->gdi_mem_dc, s->gdi_bitmap, 0,
                      static_cast<UINT>(s->cap_h),
                      last_frame.data(),
                      reinterpret_cast<BITMAPINFO*>(&bi),
                      DIB_RGB_COLORS);

            for (DWORD i = 3; i < buf_bytes; i += 4)
                last_frame[i] = 0xFF;

            have_frame = true;
        }
        
        else {
            // DXGI Desktop Duplication path (non-blocking)
            DXGI_OUTDUPL_FRAME_INFO fi = {};
            ComPtr<IDXGIResource> res;
            HRESULT hr = s->duplication->AcquireNextFrame(0, &fi, &res);

            if (SUCCEEDED(hr)) {
                if (fi.LastPresentTime.QuadPart != 0 && res) {
                    ComPtr<ID3D11Texture2D> desktop_tex;
                    if (SUCCEEDED(res.As(&desktop_tex))) {
                        D3D11_BOX box;
                        box.left   = static_cast<UINT>(s->cap_x - s->mon_x);
                        box.top    = static_cast<UINT>(s->cap_y - s->mon_y);
                        box.right  = box.left + static_cast<UINT>(s->cap_w);
                        box.bottom = box.top  + static_cast<UINT>(s->cap_h);
                        box.front  = 0;
                        box.back   = 1;

                        s->d3d_ctx->CopySubresourceRegion(
                            s->staging.Get(), 0, 0, 0, 0,
                            desktop_tex.Get(), 0, &box);

                        D3D11_MAPPED_SUBRESOURCE mapped;
                        if (SUCCEEDED(s->d3d_ctx->Map(
                                s->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                            const int row_bytes = s->cap_w * 4;
                            for (int row = 0; row < s->cap_h; ++row) {
                                const BYTE* src =
                                    static_cast<const BYTE*>(mapped.pData) +
                                    row * mapped.RowPitch;
                                BYTE* dst = last_frame.data() + row * row_bytes;
                                memcpy(dst, src, static_cast<size_t>(row_bytes));
                            }
                            s->d3d_ctx->Unmap(s->staging.Get(), 0);
                               // ── Draw cursor ──────────────────────────────────────
                        draw_cursor_dxgi(s, last_frame.data(), fi);
                        
                            have_frame = true;
                        }
                    }
                }
                s->duplication->ReleaseFrame();
            } else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                // No new desktop frame — reuse last_frame
            } else {
                break;  // fatal error
            }
        }

        if (!have_frame) continue;

        // Build an MF sample from last_frame
        ComPtr<IMFMediaBuffer> mf_buf;
        if (FAILED(MFCreateMemoryBuffer(buf_bytes, &mf_buf))) continue;

        BYTE* raw = nullptr;
        mf_buf->Lock(&raw, nullptr, nullptr);
        memcpy(raw, last_frame.data(), buf_bytes);
        mf_buf->Unlock();
        mf_buf->SetCurrentLength(buf_bytes);

        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample))) continue;
        sample->AddBuffer(mf_buf.Get());
        sample->SetSampleTime(s->vid_pts);
        sample->SetSampleDuration(kFrameDur);
        s->vid_pts += kFrameDur;

        {
            std::lock_guard<std::mutex> lock(s->mf_lock);
            s->sink_writer->WriteSample(s->vid_idx, sample.Get());
        }
    }

    ::CoUninitialize();
}

// ─── Audio capture thread ─────────────────────────────────────────────────────
static void audio_thread_func(RecorderState* s) {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (!s->stop.load()) {
        UINT32 packet_length = 0;
        if (FAILED(s->capture_client->GetNextPacketSize(&packet_length))) break;

        while (packet_length > 0) {
            BYTE*  data         = nullptr;
            UINT32 num_frames   = 0;
            DWORD  flags        = 0;
            if (FAILED(s->capture_client->GetBuffer(
                    &data, &num_frames, &flags, nullptr, nullptr))) break;

            if (num_frames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                const int ch       = s->aud_channels;
                const int sr       = s->aud_sample_rate;
                const DWORD pcm_sz = static_cast<DWORD>(num_frames) *
                                     static_cast<DWORD>(ch) * 2u;  // PCM16

                ComPtr<IMFMediaBuffer> mf_buf;
                if (SUCCEEDED(MFCreateMemoryBuffer(pcm_sz, &mf_buf))) {
                    BYTE* buf_data = nullptr;
                    mf_buf->Lock(&buf_data, nullptr, nullptr);
                    auto* dst = reinterpret_cast<int16_t*>(buf_data);

                    if (s->aud_is_float) {
                        // 32-bit float → int16
                        const auto* src = reinterpret_cast<const float*>(data);
                        const UINT32 n  = num_frames * static_cast<UINT32>(ch);
                        for (UINT32 k = 0; k < n; ++k) {
                            float v = src[k];
                            if (v >  1.0f) v =  1.0f;
                            if (v < -1.0f) v = -1.0f;
                            dst[k] = static_cast<int16_t>(v * 32767.0f);
                        }
                    } else if (s->aud_bits == 16) {
                        // Already int16 — direct copy
                        memcpy(dst, data, pcm_sz);
                    } else if (s->aud_bits == 32) {
                        // 32-bit int → int16 (keep high 16 bits)
                        const auto* src = reinterpret_cast<const int32_t*>(data);
                        const UINT32 n  = num_frames * static_cast<UINT32>(ch);
                        for (UINT32 k = 0; k < n; ++k) {
                            dst[k] = static_cast<int16_t>(src[k] >> 16);
                        }
                    } else {
                        memset(dst, 0, pcm_sz);
                    }

                    mf_buf->Unlock();
                    mf_buf->SetCurrentLength(pcm_sz);

                    LONGLONG dur = static_cast<LONGLONG>(num_frames) *
                                   10000000LL / sr;
                    ComPtr<IMFSample> sample;
                    if (SUCCEEDED(MFCreateSample(&sample))) {
                        sample->AddBuffer(mf_buf.Get());
                        sample->SetSampleTime(s->aud_pts);
                        sample->SetSampleDuration(dur);
                        s->aud_pts += dur;

                        std::lock_guard<std::mutex> lock(s->mf_lock);
                        s->sink_writer->WriteSample(s->aud_idx, sample.Get());
                    }
                }
            }

            s->capture_client->ReleaseBuffer(num_frames);
            if (FAILED(s->capture_client->GetNextPacketSize(&packet_length))) {
                packet_length = 0;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ::CoUninitialize();
}

// ─── Plugin class ─────────────────────────────────────────────────────────────
class ScreenRecorderPlugin : public flutter::Plugin {
 public:
    // Use the base PluginRegistrar* — avoids a risky reinterpret_cast and we
    // don't need any Windows-specific registrar features here.
    static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar) {
        auto plugin = std::make_unique<ScreenRecorderPlugin>(registrar);
        registrar->AddPlugin(std::move(plugin));
    }

    // The channel MUST be a member so it stays alive as long as the plugin.
    // MFStartup is deferred to the first StartRecording call.
    explicit ScreenRecorderPlugin(flutter::PluginRegistrar* registrar) {
        channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            registrar->messenger(), kChannelName,
            &flutter::StandardMethodCodec::GetInstance());
        channel_->SetMethodCallHandler(
            [this](const flutter::MethodCall<flutter::EncodableValue>& call,
                   std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
                HandleMethodCall(call, std::move(result));
            });
    }

    ~ScreenRecorderPlugin() override {
        if (g_state) {
            g_state->stop.store(true);
            if (g_state->vid_thread.joinable()) g_state->vid_thread.join();
            if (g_state->aud_thread.joinable()) g_state->aud_thread.join();
            if (g_state->has_audio && g_state->audio_client)
                g_state->audio_client->Stop();
            if (g_state->sink_writer)
                g_state->sink_writer->Finalize();
            if (g_state->mix_fmt) {
                CoTaskMemFree(g_state->mix_fmt);
                g_state->mix_fmt = nullptr;
            }
            if (g_state->gdi_bitmap)    DeleteObject(g_state->gdi_bitmap);
            if (g_state->gdi_mem_dc)    DeleteDC(g_state->gdi_mem_dc);
            if (g_state->gdi_screen_dc) ReleaseDC(nullptr, g_state->gdi_screen_dc);
            g_state.reset();
        }
        if (mf_started_) MFShutdown();
    }

 private:
    using MethodCall   = flutter::MethodCall<flutter::EncodableValue>;
    using MethodResult = flutter::MethodResult<flutter::EncodableValue>;

    std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
    bool mf_started_ = false;

    void HandleMethodCall(const MethodCall& call,
                          std::unique_ptr<MethodResult> result) {
        const auto& name = call.method_name();
        if (name == "checkPermissions") {
            result->Success(flutter::EncodableValue(true));
        } else if (name == "selectRegion") {
            // Handled by the Flutter overlay in Dart on Windows
            result->Success();
        } else if (name == "startRecording") {
            StartRecording(call, std::move(result));
        } else if (name == "stopRecording") {
            StopRecording(std::move(result));
        } else if (name == "captureDesktopScreenshot") {
            CaptureDesktopScreenshot(std::move(result));
        } else if (name == "setFullscreen") {
            const auto* args = std::get_if<bool>(call.arguments());
            SetFullscreen(args && *args, std::move(result));
        } else {
            result->NotImplemented();
        }
    }

    void StartRecording(const MethodCall& call,
                        std::unique_ptr<MethodResult> result) {
        if (g_state) {
            result->Error("ALREADY_RECORDING", "Recording already in progress");
            return;
        }

        // Initialise MF lazily on first recording (avoids touching COM at startup)
        if (!mf_started_) {
            HRESULT hr = MFStartup(MF_VERSION);
            if (FAILED(hr)) {
                result->Error("MF_INIT", "MFStartup failed");
                return;
            }
            mf_started_ = true;
        }

        const auto* args =
            std::get_if<flutter::EncodableMap>(call.arguments());
        if (!args) {
            result->Error("INVALID_ARGS", "Expected a map");
            return;
        }

        auto get_int = [&](const std::string& key) -> int {
            auto it = args->find(flutter::EncodableValue(key));
            if (it == args->end()) return 0;
            if (const auto* v = std::get_if<int>(&it->second)) return *v;
            return 0;
        };
        auto get_str = [&](const std::string& key) -> std::string {
            auto it = args->find(flutter::EncodableValue(key));
            if (it == args->end()) return {};
            if (const auto* v = std::get_if<std::string>(&it->second)) return *v;
            return {};
        };

        const int logi_x  = get_int("x");
        const int logi_y  = get_int("y");
        const int logi_w  = get_int("width");
        const int logi_h  = get_int("height");
        const std::string out_path = get_str("outputPath");

        if (logi_w <= 0 || logi_h <= 0 || out_path.empty()) {
            result->Error("INVALID_ARGS", "Invalid region or output path");
            return;
        }

        // Convert Flutter logical pixels → physical screen pixels
        // Flutter's globalPosition is relative to the window client area.
        // We need absolute physical screen coordinates for DXGI.
        UINT dpi = g_flutter_hwnd ? GetDpiForWindow(g_flutter_hwnd) : 96;
        double scale = static_cast<double>(dpi) / 96.0;

        POINT client_origin = { 0, 0 };
        if (g_flutter_hwnd) ClientToScreen(g_flutter_hwnd, &client_origin);

        int phys_x = client_origin.x + static_cast<int>(logi_x * scale);
        int phys_y = client_origin.y + static_cast<int>(logi_y * scale);
        int phys_w = static_cast<int>(logi_w * scale) & ~1;  // round down to even
        int phys_h = static_cast<int>(logi_h * scale) & ~1;

        if (phys_w < 2 || phys_h < 2) {
            result->Error("INVALID_ARGS", "Region too small after DPI scaling");
            return;
        }

        auto s = std::make_unique<RecorderState>();
        s->output_path = to_wide(out_path);
        s->cap_x = phys_x;
        s->cap_y = phys_y;
        s->cap_w = phys_w;
        s->cap_h = phys_h;

        // 1. Screen capture (DXGI Desktop Duplication → GDI BitBlt fallback)
        HRESULT hr = init_capture(s.get());
        if (FAILED(hr)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Capture init failed: 0x%08lX  region=(%d,%d %dx%d)",
                     hr, phys_x, phys_y, phys_w, phys_h);
            result->Error("CAPTURE_FAILED", msg);
            return;
        }

        // 2. WASAPI (non-fatal: fall back to video-only on failure)
        hr = init_wasapi(s.get());
        s->has_audio = SUCCEEDED(hr);

        // 3. MF Sink Writer
        hr = init_mf_writer(s.get());
        if (FAILED(hr)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "MF Sink Writer init failed: 0x%08lX", hr);
            result->Error("MF_FAILED", msg);
            return;
        }

        // 4. Start WASAPI capture
        if (s->has_audio) s->audio_client->Start();

        // 5. Launch capture threads
        s->stop = false;
        RecorderState* raw = s.get();
        s->vid_thread = std::thread(video_thread_func, raw);
        if (s->has_audio) {
            s->aud_thread = std::thread(audio_thread_func, raw);
        }

        g_state = std::move(s);
        result->Success();
    }

    // ── Capture a screenshot of the entire virtual desktop via GDI ────────────
    void CaptureDesktopScreenshot(std::unique_ptr<MethodResult> result) {
        if (!g_flutter_hwnd) {
            result->Error("NO_HWND", "Flutter window handle not available");
            return;
        }

        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        HDC hScreen = GetDC(nullptr);
        HDC hMemDC  = CreateCompatibleDC(hScreen);
        HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, vw, vh);
        HGDIOBJ hOld    = SelectObject(hMemDC, hBitmap);
        BitBlt(hMemDC, 0, 0, vw, vh, hScreen, vx, vy, SRCCOPY);
        SelectObject(hMemDC, hOld);

        BITMAPINFOHEADER bi = {};
        bi.biSize        = sizeof(bi);
        bi.biWidth       = vw;
        bi.biHeight      = -vh;  // top-down
        bi.biPlanes      = 1;
        bi.biBitCount    = 32;
        bi.biCompression = BI_RGB;

        std::vector<uint8_t> pixels(static_cast<size_t>(vw) * vh * 4);
        GetDIBits(hMemDC, hBitmap, 0, static_cast<UINT>(vh),
                  pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi),
                  DIB_RGB_COLORS);

        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(nullptr, hScreen);

        // GDI BGRX has undefined alpha — set to 0xFF for Flutter
        for (size_t i = 3; i < pixels.size(); i += 4) {
            pixels[i] = 0xFF;
        }

        flutter::EncodableMap response;
        response[flutter::EncodableValue("bytes")]  =
            flutter::EncodableValue(pixels);
        response[flutter::EncodableValue("width")]  =
            flutter::EncodableValue(vw);
        response[flutter::EncodableValue("height")] =
            flutter::EncodableValue(vh);
        result->Success(flutter::EncodableValue(response));
    }

    // ── Toggle the Flutter window between fullscreen-borderless and normal ────
    void SetFullscreen(bool fullscreen, std::unique_ptr<MethodResult> result) {
        if (!g_flutter_hwnd) {
            result->Error("NO_HWND", "Flutter window handle not available");
            return;
        }

        if (fullscreen) {
            g_prev_placement.length = sizeof(WINDOWPLACEMENT);
            GetWindowPlacement(g_flutter_hwnd, &g_prev_placement);
            g_prev_style   = GetWindowLongW(g_flutter_hwnd, GWL_STYLE);
            g_prev_exstyle = GetWindowLongW(g_flutter_hwnd, GWL_EXSTYLE);

            SetWindowLongW(g_flutter_hwnd, GWL_STYLE,
                           WS_POPUP | WS_VISIBLE);
            SetWindowLongW(g_flutter_hwnd, GWL_EXSTYLE,
                           WS_EX_TOPMOST | WS_EX_APPWINDOW);

            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

            SetWindowPos(g_flutter_hwnd, HWND_TOPMOST,
                         vx, vy, vw, vh,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else {
            SetWindowLongW(g_flutter_hwnd, GWL_STYLE, g_prev_style);
            SetWindowLongW(g_flutter_hwnd, GWL_EXSTYLE, g_prev_exstyle);
            SetWindowPlacement(g_flutter_hwnd, &g_prev_placement);
            SetWindowPos(g_flutter_hwnd, HWND_NOTOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        }

        result->Success();
    }

    void StopRecording(std::unique_ptr<MethodResult> result) {
        if (!g_state) {
            result->Error("NOT_RECORDING", "No active recording");
            return;
        }

        RecorderState* s = g_state.get();
        s->stop.store(true);

        if (s->vid_thread.joinable()) s->vid_thread.join();
        if (s->aud_thread.joinable()) s->aud_thread.join();

        if (s->has_audio && s->audio_client) s->audio_client->Stop();

        {
            std::lock_guard<std::mutex> lock(s->mf_lock);
            s->sink_writer->Finalize();
        }

        if (s->mix_fmt) {
            CoTaskMemFree(s->mix_fmt);
            s->mix_fmt = nullptr;
        }

        // Clean up GDI resources if we used the fallback
        if (s->gdi_bitmap)    { DeleteObject(s->gdi_bitmap);          s->gdi_bitmap = nullptr; }
        if (s->gdi_mem_dc)    { DeleteDC(s->gdi_mem_dc);             s->gdi_mem_dc = nullptr; }
        if (s->gdi_screen_dc) { ReleaseDC(nullptr, s->gdi_screen_dc); s->gdi_screen_dc = nullptr; }

        std::string out = from_wide(s->output_path);
        g_state.reset();

        result->Success(flutter::EncodableValue(out));
    }
};

// ─── Public registration entry point ─────────────────────────────────────────
void ScreenRecorderPluginRegisterWithRegistrar(
    flutter::PluginRegistrar* registrar,
    HWND flutter_hwnd) {
    g_flutter_hwnd = flutter_hwnd;
    ScreenRecorderPlugin::RegisterWithRegistrar(registrar);
}
