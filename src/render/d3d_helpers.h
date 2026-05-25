#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>

namespace psvr2pt {

template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

inline void check_hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string("D3D11 ") + what + " failed: 0x"
            + [hr]{ char b[16]; sprintf_s(b, "%08X", hr); return std::string(b); }());
    }
}

}  // namespace psvr2pt
