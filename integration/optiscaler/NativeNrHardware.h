#pragma once
// B580 is tested locally; B570 shares Xe2 but remains hardware-unvalidated.
constexpr bool NativeNrSupportedAdapter(unsigned vendor,unsigned device) noexcept {
    return vendor==0x8086 && (device==0xe20b || device==0xe20c);
}
