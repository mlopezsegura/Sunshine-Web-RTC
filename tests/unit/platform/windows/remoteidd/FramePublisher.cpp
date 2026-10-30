// Copia de OpenStreamMS/Native/RemoteIdd (contrato con el driver RemoteIdd). No editar aquí:
// cambiar en OpenStreamMS y volver a copiar, para que productor y consumidor no diverjan.
// FramePublisher.cpp — ver FramePublisher.h.

#include "FramePublisher.h"
#include <dxgi1_2.h>
#include <sddl.h>
#include <cstdio>
#include <utility>

using namespace Microsoft::WRL;
namespace osms::ridd {

static void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[256];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
}

FramePublisher::FramePublisher(ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context,
                               UINT32 sessionId, const wchar_t* ns, const wchar_t* sddl)
    : m_device(std::move(device)), m_context(std::move(context)),
      m_sessionId(sessionId), m_sddl(sddl)
{
    wcsncpy_s(m_ns, ns, _TRUNCATE);
}

FramePublisher::~FramePublisher()
{
    m_sharedMutex.Reset();
    m_shared.Reset();
    if (m_sharedHandle) CloseHandle(m_sharedHandle);
    CloseSharedObjects();
}

void FramePublisher::CloseSharedObjects()
{
    if (m_slot) { UnmapViewOfFile(m_slot); m_slot = nullptr; }
    if (m_slotMap) { CloseHandle(m_slotMap); m_slotMap = nullptr; }
    if (m_evReady) { CloseHandle(m_evReady); m_evReady = nullptr; }
    if (m_evDone) { CloseHandle(m_evDone); m_evDone = nullptr; }
}

void FramePublisher::SetSession(UINT32 sessionId)
{
    if (sessionId == m_sessionId) return;
    CloseSharedObjects();
    m_sessionId = sessionId;
}

bool FramePublisher::EnsureSharedObjects()
{
    if (m_slot && m_evReady && m_evDone) return true;

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (m_sddl && !ConvertStringSecurityDescriptorToSecurityDescriptorW(m_sddl, SDDL_REVISION_1, &sd, nullptr))
        return false;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    SECURITY_ATTRIBUTES* psa = sd ? &sa : nullptr;

    wchar_t name[128];
    if (!m_slotMap)
    {
        swprintf_s(name, REMOTEIDD_FMT_SLOT, m_ns, m_sessionId);
        m_slotMap = CreateFileMappingW(INVALID_HANDLE_VALUE, psa, PAGE_READWRITE, 0,
                                       sizeof(RemoteIddSharedSlot), name);
    }
    if (m_slotMap && !m_slot)
        m_slot = reinterpret_cast<RemoteIddSharedSlot*>(
            MapViewOfFile(m_slotMap, FILE_MAP_WRITE, 0, 0, sizeof(RemoteIddSharedSlot)));
    if (!m_evReady)
    {
        swprintf_s(name, REMOTEIDD_FMT_READY, m_ns, m_sessionId);
        m_evReady = CreateEventW(psa, FALSE, FALSE, name);
    }
    if (!m_evDone)
    {
        swprintf_s(name, REMOTEIDD_FMT_DONE, m_ns, m_sessionId);
        m_evDone = CreateEventW(psa, FALSE, FALSE, name);
    }
    if (sd) LocalFree(sd);

    if (!m_slot || !m_evReady || !m_evDone)
    {
        Log(L"RemoteIdd: no se pudieron crear los objetos de SharedFrame (err %lu)\n", GetLastError());
        return false;   // se reintenta en el siguiente frame; lo ya creado se reutiliza
    }
    m_slot->Version = REMOTEIDD_SHARED_VERSION;
    m_slot->SessionId = m_sessionId;
    m_info.ProducerPid = GetCurrentProcessId();
    return true;
}

bool FramePublisher::EnsureSharedTexture(const D3D11_TEXTURE2D_DESC& src)
{
    if (m_shared)
    {
        D3D11_TEXTURE2D_DESC cur;
        m_shared->GetDesc(&cur);
        if (cur.Width == src.Width && cur.Height == src.Height && cur.Format == src.Format)
            return true;
    }

    // Cambio de tamaño/formato (o primer frame): se recrea y sube TextureGeneration.
    m_sharedMutex.Reset();
    m_shared.Reset();
    if (m_sharedHandle) { CloseHandle(m_sharedHandle); m_sharedHandle = nullptr; }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = src.Width;
    desc.Height = src.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = src.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ComPtr<ID3D11Texture2D> tex;
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<IDXGIResource1> res;
    HANDLE h = nullptr;
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &tex)) ||
        FAILED(tex.As(&mutex)) || FAILED(tex.As(&res)) ||
        FAILED(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                       nullptr, &h)))
    {
        Log(L"RemoteIdd: no se pudo crear la textura compartida %ux%u fmt %u\n",
            src.Width, src.Height, static_cast<unsigned>(src.Format));
        return false;
    }

    m_shared = tex;
    m_sharedMutex = mutex;
    m_sharedHandle = h;
    m_textureChanged = true;
    m_info.TextureGeneration++;
    return true;
}

FramePublisher::Result FramePublisher::Publish(ID3D11Texture2D* src, UINT64 qpcPresent)
{
    if (!src || !EnsureSharedObjects()) return Result::Failed;

    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);
    if (!EnsureSharedTexture(desc)) return Result::Failed;

    // Limitador por llegada también en la GPU: si el consumidor aún tiene la textura
    // (keyed mutex ocupado), este frame se pierde. AcquireSync con timeout 0 nunca bloquea.
    HRESULT hr = m_sharedMutex->AcquireSync(0, 0);
    if (hr != S_OK && hr != static_cast<HRESULT>(WAIT_ABANDONED)) return Result::Dropped;
    m_context->CopyResource(m_shared.Get(), src);
    m_sharedMutex->ReleaseSync(0);

    m_info.Width = desc.Width;
    m_info.Height = desc.Height;
    m_info.DxgiFormat = static_cast<uint32_t>(desc.Format);
    m_info.SharedTextureHandle = reinterpret_cast<uint64_t>(m_sharedHandle);
    m_info.FrameId++;
    m_info.QpcPresent = qpcPresent;
    m_info.Flags = m_textureChanged ? REMOTEIDD_FLAG_TEXTURE_CHANGED : 0;
    m_textureChanged = false;

    RemoteIddSlotPublish(m_slot, m_info);
    SetEvent(m_evReady);
    return Result::Published;
}

} // namespace osms::ridd
