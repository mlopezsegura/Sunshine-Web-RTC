// Copia de OpenStreamMS/Native/RemoteIdd (contrato con el driver RemoteIdd). No editar aquí:
// cambiar en OpenStreamMS y volver a copiar, para que productor y consumidor no diverjan.
// FrameConsumer.h — lado consumidor del contrato SharedFrame.h (lo usa Sunshine).
//
// Uso por frame:
//   if (c.WaitFrame(ms) == Wait::Frame && c.Acquire(ms, &tex, &info)) {
//       ... copiar/codificar tex en el dispositivo propio ...
//       c.Release();
//   }
// Entre Acquire y Release el productor no puede escribir la textura: conviene copiarla
// a una textura propia y soltarla enseguida, o el driver perderá frames.
#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include "SharedFrame.h"

namespace osms::ridd
{
    class FrameConsumer
    {
    public:
        enum class Wait { Frame, Timeout, Error };

        FrameConsumer(Microsoft::WRL::ComPtr<ID3D11Device> device, UINT32 sessionId,
                      const wchar_t* ns = REMOTEIDD_NS);
        ~FrameConsumer();

        FrameConsumer(const FrameConsumer&) = delete;
        FrameConsumer& operator=(const FrameConsumer&) = delete;

        // Abre slot y eventos. false si el productor aún no los ha creado (reintentar).
        bool Open();
        Wait WaitFrame(DWORD timeoutMs);

        // Lee el descriptor más reciente, (re)abre la textura si cambió y toma el keyed
        // mutex. La textura sigue siendo del consumidor hasta Release().
        bool Acquire(DWORD timeoutMs, ID3D11Texture2D** texture, RemoteIddFrameInfo* info);
        void Release();

    private:
        bool OpenTexture(const RemoteIddFrameInfo& info);

        Microsoft::WRL::ComPtr<ID3D11Device1> m_device;
        UINT32 m_sessionId;
        wchar_t m_ns[16] = {};

        HANDLE m_slotMap = nullptr;
        const RemoteIddSharedSlot* m_slot = nullptr;
        HANDLE m_evReady = nullptr;
        HANDLE m_evDone = nullptr;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_mutex;
        UINT32 m_producerPid = 0;
        UINT32 m_generation = 0;
        bool m_held = false;
    };
}
