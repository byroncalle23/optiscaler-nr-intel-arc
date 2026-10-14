#pragma once
#include <atomic>
#include <imgui/imgui.h>
#include "NativeNrModes.h"

// Request only: the render callback owns initialization and GPU resources.
// -1 is an unread startup setting. Menu changes never persist to the INI.
namespace NativeNrControl {
inline std::atomic<int> requested{-1};
inline std::atomic<int> progress{0}; // 0 idle, 1 preparing, 2 recording, 3 failed
inline std::atomic<bool> temporalRequested{false};
inline std::atomic<bool> temporalApplied{false};
inline std::atomic<int> temporalProgress{0}; // 0 missing inputs, 1 preparing, 2 captured, 3 unsupported
inline void Startup(bool enabled){int unset=-1;requested.compare_exchange_strong(unset,enabled?1:0);}
inline bool Enabled(){return requested.load(std::memory_order_acquire)==1;}
inline void Set(bool enabled){requested.store(enabled?1:0,std::memory_order_release);}
}

bool NativeNrIsEnabled();
void NativeNrSetEnabled(bool enabled);

// Same widget in the game and in the headless ImGui interaction test.
inline bool DrawNativeNrCheckbox(){
    ImGui::Separator();
    bool enabled=NativeNrIsEnabled();
    if(ImGui::Checkbox("Reconstruccion neuronal experimental",&enabled))NativeNrSetEnabled(enabled);
    return enabled;
}
inline void RenderNativeNrControl(){
    bool enabled=DrawNativeNrCheckbox();
#ifdef NR_GAME_MODES
    int selected=NativeNrModes::requested.load();
    if(ImGui::BeginCombo("Modo neuronal",NativeNrModes::modes[selected].label)){
        for(int i=0;i<5;++i)if(ImGui::Selectable(NativeNrModes::modes[i].label,selected==i))NativeNrModes::Set(i);
        ImGui::EndCombo();
    }
    if(enabled&&NativeNrModes::failure.load()==1)ImGui::TextWrapped("VRAM disponible insuficiente para este modo. Elige uno menor o reduce el uso de memoria del juego.");
    else if(enabled&&NativeNrModes::failure.load()==2)ImGui::TextWrapped("Cambio pendiente: la grabacion anterior no termino. Elige otro modo para reintentar.");
    else if(enabled&&NativeNrModes::active.load()!=NativeNrModes::requested.load()&&NativeNrModes::failure.load()==0)ImGui::TextWrapped("Preparando el modo elegido; el historial se reiniciara.");
#endif
    if(!enabled)ImGui::TextUnformatted("NR desactivado. Puedes activarlo durante la partida.");
    else if(NativeNrControl::progress.load()==3)ImGui::TextWrapped("NR encontro un error. Consulta OptiScaler.log.");
    else if(NativeNrControl::progress.load()==2)ImGui::TextUnformatted("NR: pase enviado en este Present.");
    else ImGui::TextUnformatted("NR solicitado: preparando el modelo cuando haya una salida compatible.");
    if(enabled&&NativeNrControl::temporalRequested.load()){
        if(NativeNrControl::temporalApplied.load())ImGui::TextUnformatted("Temporal aplicado al pase NR.");
        else if(NativeNrControl::temporalProgress.load()==2)ImGui::TextUnformatted("Temporal: movimiento y profundidad recibidos.");
        else if(NativeNrControl::temporalProgress.load()==1)ImGui::TextUnformatted("Temporal: preparando historial; NR conserva la ruta anterior.");
        else ImGui::TextWrapped("Temporal no aplicado: NR usa la ruta anterior. Consulta OptiScaler.log.");
    }
#ifdef NR_GAME_GLOBAL_B1
#ifdef NR_GAME_MODES
    ImGui::TextWrapped("Un pase sincrono, temporal y filtro de bordes. El modo elegido dura solo esta partida.");
#elif defined(NR_GAME_COMPACT320)
    ImGui::TextWrapped("PRUEBA compacta: 320x180, un pase sincrono, temporal y EdgeGuidedResidual sobre SDR nativo.");
#elif defined(NR_GAME_COMPACT512)
    ImGui::TextWrapped("PRUEBA compacta: 512x288, un pase sincrono y EdgeGuidedResidual sobre SDR nativo.");
#else
    ImGui::TextWrapped("Global B1: 640x360, un pase y residual sobre SDR nativo. El cambio dura solo esta partida.");
#endif
#else
    ImGui::TextWrapped("El modo detalle es muy lento. El cambio dura solo esta partida.");
#endif
}
