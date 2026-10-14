#define NR_GAME_MODES
#include "imgui.h"
#include "imgui_internal.h"
#include "NativeNrControl.h"
#include <cstdio>
#include <stdexcept>
bool NativeNrIsEnabled(){return NativeNrControl::Enabled();}
void NativeNrSetEnabled(bool enabled){NativeNrControl::Set(enabled);}
int main(){try{
    NativeNrControl::Startup(false);
    if(NativeNrIsEnabled())throw std::runtime_error("Startup enabled unexpectedly");
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize=ImVec2(900,600);io.DeltaTime=1.f/60;
    io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures;
    ImRect widget;
    auto frame=[&](){ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(800,500));
        ImGui::Begin("NR menu test");
        // Same checkbox renderer as OptiScaler. Find its rect before later texts.
        DrawNativeNrCheckbox();
        widget=GImGui->LastItemData.Rect;
        ImGui::End();ImGui::Render();};
    frame();
    auto click=[&](){auto center=widget.GetCenter();io.AddMousePosEvent(center.x,center.y);frame();
        io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();};
    click();if(!NativeNrIsEnabled())throw std::runtime_error("Checkbox failed to enable");
    NativeNrControl::Startup(false);if(!NativeNrIsEnabled())throw std::runtime_error("Startup overwrote user request");
    click();if(NativeNrIsEnabled())throw std::runtime_error("Checkbox failed to disable");
    // Complete shared renderer must also generate valid draw data.
    ImGui::NewFrame();ImGui::Begin("Complete control");RenderNativeNrControl();ImGui::End();ImGui::Render();
    if(!ImGui::GetDrawData()->Valid)throw std::runtime_error("Invalid menu draw data");
    if(NativeNrModes::requested.load()!=0)throw std::runtime_error("Performance mode must be default");
    for(int mode=0;mode<5;++mode){
        if(!NativeNrModes::Set(mode))throw std::runtime_error("Valid mode rejected");
        auto geometry=NativeNrModes::modes[mode];
        if(geometry.width*9!=geometry.height*16||geometry.tokens>640)throw std::runtime_error("Mode geometry invalid");
        ImGui::NewFrame();ImGui::Begin("Mode control");RenderNativeNrControl();ImGui::End();ImGui::Render();
        if(!ImGui::GetDrawData()->Valid)throw std::runtime_error("Invalid mode draw data");
    }
    if(NativeNrModes::Set(-1)||NativeNrModes::Set(5)||NativeNrModes::requested.load()!=4)throw std::runtime_error("Invalid mode changed request");
    puts("FIVE_MODE_RENDER_DEFAULT_AND_INVALID_SELECTION_PASS");
    ImGui::DestroyContext();puts("{\"passed\":true,\"startup_disabled\":true,\"checkbox_enable_disable\":true,\"request_not_overwritten\":true}");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
