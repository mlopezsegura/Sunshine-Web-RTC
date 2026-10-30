// Copia de OpenStreamMS/Native/RemoteIdd (contrato con el driver RemoteIdd). No editar aquí:
// cambiar en OpenStreamMS y volver a copiar, para que productor y consumidor no diverjan.
// SharedFrame.h — contrato de entrega de frames RemoteIdd -> Sunshine.
//
// Independiente del protocolo remoto: define cómo el driver publica cada frame del
// swapchain y cómo Sunshine lo consume. Modelado sobre lo observado en DuoIdd
// (objetos con nombre "Global\\DuoIdd%uFrameReady" / "...FrameEncoded"), pero con un
// contrato explícito y versionado.
//
// Un frame NO se copia por este header: se comparte la textura por handle NT y aquí
// viaja solo el descriptor + sincronización. Evita una copia en CPU, igual que hace el
// presentador entre sesiones de OpenStreamMS.
//
// Invasividad: solo objetos con nombre que viven mientras viva el driver (sin registro,
// sin ficheros, sin servicios). Su DACL se limita a SYSTEM y LocalService.
#pragma once
#include <windows.h>
#include <cstdint>

#define REMOTEIDD_SHARED_VERSION 2u   // v2: seqlock (Seq) + RemoteIddFrameInfo

// Prefijo del espacio de nombres. "Global\\" porque productor (WUDFHost, sesión 0) y
// consumidor (Sunshine, sesión del puesto) no comparten sesión. Crear objetos en
// Global\ exige SeCreateGlobalPrivilege (lo tienen los servicios, no un usuario normal);
// el test lo redefine a "Local\\" para correr sin privilegios.
#ifndef REMOTEIDD_NS
#define REMOTEIDD_NS L"Global\\"
#endif

// Nombres de los objetos, por sesión de Terminal Services:
//   <NS>OsmsRidd<session>Slot       -> file mapping con RemoteIddSharedSlot
//   <NS>OsmsRidd<session>FrameReady -> auto-reset event, lo pone el driver
//   <NS>OsmsRidd<session>FrameDone  -> auto-reset event, lo pone Sunshine
#define REMOTEIDD_NAME_SLOT   REMOTEIDD_NS L"OsmsRidd%uSlot"
#define REMOTEIDD_NAME_READY  REMOTEIDD_NS L"OsmsRidd%uFrameReady"
#define REMOTEIDD_NAME_DONE   REMOTEIDD_NS L"OsmsRidd%uFrameDone"

// Las mismas, con el prefijo en tiempo de ejecución (FramePublisher / FrameConsumer):
// swprintf_s(buf, REMOTEIDD_FMT_SLOT, ns, session).
#define REMOTEIDD_FMT_SLOT    L"%lsOsmsRidd%uSlot"
#define REMOTEIDD_FMT_READY   L"%lsOsmsRidd%uFrameReady"
#define REMOTEIDD_FMT_DONE    L"%lsOsmsRidd%uFrameDone"

// DACL de los objetos con nombre: SYSTEM (Sunshine se lanza como SYSTEM en la sesión del
// puesto), LocalService (cuenta del WUDFHost) y administradores (diagnóstico con
// RemoteIddPerf watch). Protegida: no hereda nada más; un usuario normal no entra.
#define REMOTEIDD_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)"

// Descriptor del frame más reciente. Se escribe y se lee SIEMPRE con
// RemoteIddSlotPublish / RemoteIddSlotRead, nunca campo a campo.
typedef struct _RemoteIddFrameInfo
{
    uint32_t Width;          // ancho actual en píxeles
    uint32_t Height;         // alto actual en píxeles
    uint32_t RefreshNumer;   // Hz = Numer/Denom (p. ej. 60000/1000)
    uint32_t RefreshDenom;
    uint32_t DxgiFormat;     // DXGI_FORMAT del buffer (p. ej. 87 = B8G8R8A8_UNORM)
    uint32_t ProducerPid;    // pid del WUDFHost del driver, para DuplicateHandle

    // Handle NT compartido de la textura (con keyed mutex, clave 0), VÁLIDO EN EL PROCESO
    // DEL PRODUCTOR. El consumidor lo duplica con DuplicateHandle desde ProducerPid y lo
    // abre con ID3D11Device1::OpenSharedResource1. Así el driver no necesita conocer ni
    // abrir el proceso de Sunshine. Cambia solo al cambiar de tamaño/formato (bit0 Flags).
    uint64_t SharedTextureHandle;

    uint64_t FrameId;        // monotónico; el consumidor detecta saltos/pérdidas
    uint64_t QpcPresent;     // QueryPerformanceCounter del momento de present
    uint32_t Flags;          // bit0 = textura/modo cambió desde el frame anterior
    // Sube cada vez que se recrea la textura. El consumidor reabre por generación y no
    // por valor de handle: un handle nuevo puede reutilizar el valor del anterior.
    uint32_t TextureGeneration;
} RemoteIddFrameInfo;

#define REMOTEIDD_FLAG_TEXTURE_CHANGED 0x1u

#pragma pack(push, 8)
typedef struct _RemoteIddSharedSlot
{
    uint32_t Version;        // REMOTEIDD_SHARED_VERSION
    uint32_t SessionId;      // sesión de TS a la que pertenece este slot
    volatile LONG64 Seq;     // seqlock: impar = escritura en curso
    RemoteIddFrameInfo Frame;
} RemoteIddSharedSlot;
#pragma pack(pop)

// Productor (un único escritor). Las dos InterlockedIncrement64 son barreras completas.
inline void RemoteIddSlotPublish(RemoteIddSharedSlot* slot, const RemoteIddFrameInfo& f)
{
    InterlockedIncrement64(&slot->Seq);          // -> impar
    slot->Frame = f;
    InterlockedIncrement64(&slot->Seq);          // -> par
}

// Consumidor. Copia un descriptor coherente; false si tras `maxTries` el productor
// seguía escribiendo (prácticamente imposible: una escritura son ~60 bytes).
inline bool RemoteIddSlotRead(const RemoteIddSharedSlot* slot, RemoteIddFrameInfo* out,
                              int maxTries = 1000)
{
    for (int i = 0; i < maxTries; ++i)
    {
        // Lecturas volatile + barreras completas: válido en MSVC y MinGW (Sunshine), que no
        // tiene ReadAcquire64. El coste (~decenas de ns por frame) es despreciable.
        LONG64 before = slot->Seq;
        MemoryBarrier();
        if (before & 1) { YieldProcessor(); continue; }
        *out = slot->Frame;
        MemoryBarrier();
        if (slot->Seq == before) return true;
    }
    return false;
}

// Protocolo de sincronización (sin bloqueo del lado productor):
//   Driver: copia el frame a la textura compartida (si el keyed mutex está libre),
//           RemoteIddSlotPublish(...), SetEvent(Ready).
//   Sunshine: WaitForSingleObject(Ready) -> RemoteIddSlotRead -> AcquireSync(0) ->
//           codifica -> ReleaseSync(0) -> SetEvent(Done).
//   El driver NO espera a Done ni al keyed mutex (AcquireSync con timeout 0): si
//   Sunshine va lento, se pierde el frame intermedio, nunca se bloquea la sesión.
