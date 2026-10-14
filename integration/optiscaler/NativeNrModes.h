#pragma once
#include <atomic>
namespace NativeNrModes {
struct Mode {const char* label;unsigned width,height,tokens;const char* folder;};
inline constexpr Mode modes[]={
 {"Rendimiento - 320x180",320,180,32,"assets-180"},
 {"Equilibrado - 512x288",512,288,64,"assets-288"},
 {"Calidad - 640x360",640,360,96,"assets-360"},
 {"Calidad superior - 960x540",960,540,192,"assets-540"},
 {"Maxima resolucion - 1280x720",1280,720,240,"assets-720"}};
inline std::atomic<int> requested{0},active{-1};
inline std::atomic<int> failure{0}; // 1 VRAM budget, 2 pending recording, 3 setup error
inline bool Set(int mode){if(mode<0||mode>=5)return false;requested.store(mode);return true;}
inline bool Geometry(unsigned w,unsigned h){for(auto m:modes)if(m.width==w&&m.height==h)return true;return false;}
inline unsigned Tokens(unsigned w,unsigned h){for(auto m:modes)if(m.width==w&&m.height==h)return m.tokens;return 0;}
}
