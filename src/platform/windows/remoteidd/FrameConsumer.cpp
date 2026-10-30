// Copia de OpenStreamMS/Native/RemoteIdd (contrato con el driver RemoteIdd). No editar aquí:
// cambiar en OpenStreamMS y volver a copiar, para que productor y consumidor no diverjan.
// FrameConsumer.cpp — ver FrameConsumer.h.

#include "FrameConsumer.h"
#include <dxgi1_2.h>
#include <cstdio>

using namespace Microsoft::WRL;
namespace osms::ridd {

FrameConsumer::FrameConsumer(ComPtr<ID3D11Device> device, UINT32 sessionId, const wchar_t* ns)
    : m_sessionId(sessionId)
{
    device.As(&m_device);
    wcsncpy_s(m_ns, ns, _TRUNCATE);
}

FrameConsumer::~FrameConsumer()
{
    if (m_held) Release();
    m_mutex.Reset();
    m_texture.Reset();
    if (m_slot) UnmapViewOfFile(m_slot);
    if (m_slotMap) CloseHandle(m_slotMap);
    if (m_evReady) CloseHandle(m_evReady);
    if (m_evDone) CloseHandle(m_evDone);
}

bool FrameConsumer::Open()
{
    if (m_slot) return true;
    if (!m_device) return false;

    // Solo se abren objetos existentes: el consumidor nunca crea nada con nombre.
    wchar_t name[128];
    swprintf_s(name, REMOTEIDD_FMT_SLOT, m_ns, m_sessionId);
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!map) return false;
    auto* slot = reinterpret_cast<const RemoteIddSharedSlot*>(
        MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(RemoteIddSharedSlot)));
    swprintf_s(name, REMOTEIDD_FMT_READY, m_ns, m_sessionId);
    HANDLE ready = OpenEventW(SYNCHRONIZE, FALSE, name);
    swprintf_s(name, REMOTEIDD_FMT_DONE, m_ns, m_sessionId);
    HANDLE done = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);

    if (!slot || !ready || !done || slot->Version != REMOTEIDD_SHARED_VERSION)
    {
        if (slot) UnmapViewOfFile(slot);
        CloseHandle(map);
        if (ready) CloseHandle(ready);
        if (done) CloseHandle(done);
        return false;
    }
    m_slotMap = map;
    m_slot = slot;
    m_evReady = ready;
    m_evDone = done;
    return true;
}

FrameConsumer::Wait FrameConsumer::WaitFrame(DWORD timeoutMs)
{
    if (!m_evReady) return Wait::Error;
    switch (WaitForSingleObject(m_evReady, timeoutMs))
    {
    case WAIT_OBJECT_0: return Wait::Frame;
    case WAIT_TIMEOUT:  return Wait::Timeout;
    default:            return Wait::Error;
    }
}

bool FrameConsumer::OpenTexture(const RemoteIddFrameInfo& info)
{
    if (m_texture && info.ProducerPid == m_producerPid && info.TextureGeneration == m_generation)
        return true;

    m_mutex.Reset();
    m_texture.Reset();

    // El handle es del proceso del productor: se duplica a este proceso, se abre la
    // textura y se cierra la copia (la textura abierta mantiene su propia referencia).
    HANDLE producer = OpenProcess(PROCESS_DUP_HANDLE, FALSE, info.ProducerPid);
    if (!producer) return false;
    HANDLE h = nullptr;
    BOOL dup = DuplicateHandle(producer, reinterpret_cast<HANDLE>(info.SharedTextureHandle),
                               GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(producer);
    if (!dup) return false;

    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = m_device->OpenSharedResource1(h, IID_PPV_ARGS(&tex));
    CloseHandle(h);
    if (FAILED(hr) || FAILED(tex.As(&m_mutex))) return false;

    m_texture = tex;
    m_producerPid = info.ProducerPid;
    m_generation = info.TextureGeneration;
    return true;
}

bool FrameConsumer::Acquire(DWORD timeoutMs, ID3D11Texture2D** texture, RemoteIddFrameInfo* info)
{
    if (!m_slot || m_held) return false;

    RemoteIddFrameInfo f;
    if (!RemoteIddSlotRead(m_slot, &f) || !f.FrameId) return false;
    if (!OpenTexture(f)) return false;

    HRESULT hr = m_mutex->AcquireSync(0, timeoutMs);
    if (hr != S_OK && hr != static_cast<HRESULT>(WAIT_ABANDONED)) return false;
    m_held = true;

    // Si la textura se recreó mientras esperábamos, el contenido no es el de `f`; se
    // suelta y el siguiente WaitFrame traerá la generación nueva.
    RemoteIddFrameInfo now;
    if (RemoteIddSlotRead(m_slot, &now) && now.TextureGeneration != f.TextureGeneration)
    {
        Release();
        return false;
    }

    *texture = m_texture.Get();
    (*texture)->AddRef();
    *info = f;
    return true;
}

void FrameConsumer::Release()
{
    if (!m_held) return;
    m_mutex->ReleaseSync(0);
    m_held = false;
    SetEvent(m_evDone);
}

} // namespace osms::ridd
