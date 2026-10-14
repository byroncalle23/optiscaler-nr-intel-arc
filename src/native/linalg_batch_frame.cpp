#define NOMINMAX
#include <windows.h>
extern "C" {__declspec(dllexport) extern const UINT D3D12SDKVersion=721;__declspec(dllexport) extern const char* D3D12SDKPath=".\\D3D12\\";}
#define NR_EXPERIMENTAL_LINALG
#include "dml_batch_frame.cpp"
