// Copia de OpenStreamMS/Native/RemoteIdd (contrato con el driver RemoteIdd). No editar aquí:
// cambiar en OpenStreamMS y volver a copiar, para que productor y consumidor no diverjan.
// FramePublisher.h — lado productor del contrato SharedFrame.h.
//
// Independiente de IddCx: el driver le pasa cada buffer del swapchain y el test de
// rendimiento le pasa texturas propias, así se mide exactamente el mismo código.
// Copia el frame a una textura compartida (handle NT + keyed mutex) y lo publica en el
// slot con seqlock. Nunca bloquea: si el consumidor tiene la textura, el frame se pierde.
#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <src/platform/windows/remoteidd/SharedFrame.h>

namespace osms::ridd
{
    class FramePublisher
    {
    public:
        enum class Result { Published, Dropped, Failed };

        // ns: REMOTEIDD_NS en el driver; L"Local\\" en pruebas sin privilegios.
        // sddl: DACL de los objetos con nombre; nullptr = seguridad por defecto.
        FramePublisher(Microsoft::WRL::ComPtr<ID3D11Device> device,
                       Microsoft::WRL::ComPtr<ID3D11DeviceContext> context,
                       UINT32 sessionId,
                       const wchar_t* ns = REMOTEIDD_NS,
                       const wchar_t* sddl = REMOTEIDD_SDDL);
        ~FramePublisher();

        FramePublisher(const FramePublisher&) = delete;
        FramePublisher& operator=(const FramePublisher&) = delete;

        Result Publish(ID3D11Texture2D* src, UINT64 qpcPresent);

        // Cambia la sesión de los objetos con nombre (la da el proveedor de protocolo
        // después de cargar el driver). Se recrean en el siguiente Publish.
        void   SetSession(UINT32 sessionId);

        void   SetRefresh(UINT32 numer, UINT32 denom) { m_info.RefreshNumer = numer; m_info.RefreshDenom = denom; }
        UINT64 LastFrameId() const { return m_info.FrameId; }

    private:
        bool EnsureSharedObjects();
        void CloseSharedObjects();
        bool EnsureSharedTexture(const D3D11_TEXTURE2D_DESC& src);

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        UINT32 m_sessionId;
        wchar_t m_ns[16] = {};
        const wchar_t* m_sddl;

        HANDLE m_slotMap = nullptr;
        RemoteIddSharedSlot* m_slot = nullptr;
        HANDLE m_evReady = nullptr;
        HANDLE m_evDone = nullptr;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_shared;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_sharedMutex;
        HANDLE m_sharedHandle = nullptr;
        bool m_textureChanged = false;

        RemoteIddFrameInfo m_info = {};
    };
}
