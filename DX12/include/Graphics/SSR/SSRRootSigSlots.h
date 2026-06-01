#pragma once

// Shared compute root-signature slot layout for every SSR pass.
//
// Mirrors GraphicsDX12::CreateComputeRootSignature; the SSR sub-passes are
// "well-behaved compute" — they bind into the shared root sig (space2) and
// none of them needs slots beyond u2/t7. Constants live in an anonymous
// namespace inside the header so every TU that includes it gets its own copy
// without leaking symbols.

#include <cstdint>

namespace SSR { namespace RootSig {

inline constexpr uint32_t kCB      = 0;
inline constexpr uint32_t kSRV_T0  = 1;
inline constexpr uint32_t kSRV_T1  = 2;
inline constexpr uint32_t kSRV_T2  = 3;
inline constexpr uint32_t kSRV_T3  = 7;
inline constexpr uint32_t kSRV_T4  = 8;
inline constexpr uint32_t kSRV_T5  = 6;
inline constexpr uint32_t kSRV_T6  = 12;
inline constexpr uint32_t kSRV_T7  = 13;
inline constexpr uint32_t kUAV_U0  = 4;
inline constexpr uint32_t kUAV_U1  = 5;
inline constexpr uint32_t kUAV_U2  = 14;

}} // namespace SSR::RootSig
