#include "native/alloc/windows_dynamic_stack_capture.h"
#include "native/alloc/windows_permanent_iat_gateway.h"

#ifndef _WIN32
#error "windows_permanent_iat_gateway_unwind_test.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using spark::captureDynamicAwareStackBackTrace;
using spark::permanent_iat_gateway::bindPermanentIatGateway;
using spark::permanent_iat_gateway::createPermanentIatGateway;
using spark::permanent_iat_gateway::detachPermanentIatGateway;
using spark::permanent_iat_gateway::PermanentIatGatewayHandle;

namespace {

using FiveArgFn = std::uint64_t(__cdecl *)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

constexpr std::uint64_t KHandlerBias = 0x100000000ULL;
constexpr std::uint64_t KDrainTimeoutMs = 5000;
constexpr std::size_t KStackCapacity = 64;

std::array<void *, KStackCapacity> GFrames{};
USHORT GDepth = 0;

[[nodiscard]] std::uint64_t baseValue(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                                      std::uint64_t e) noexcept
{
    return a + 3 * b + 5 * c + 7 * d + 11 * e;
}

extern "C" __declspec(noinline) std::uint64_t __cdecl originalFive(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                   std::uint64_t d, std::uint64_t e) noexcept
{
    return baseValue(a, b, c, d, e);
}

extern "C"
    __declspec(noinline) std::uint64_t __cdecl capturingHandler(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                                std::uint64_t d, std::uint64_t e) noexcept
{
    GFrames.fill(nullptr);
    GDepth = captureDynamicAwareStackBackTrace(0, static_cast<ULONG>(GFrames.size()), GFrames.data(), nullptr);
    return baseValue(a, b, c, d, e) + KHandlerBias;
}

extern "C" __declspec(noinline) std::uint64_t __cdecl knownCaller(FiveArgFn function) noexcept
{
    volatile std::uint64_t result = function(10, 20, 30, 40, 50);
    return result;
}

[[noreturn]] void fail(const char *reason)
{
    std::fprintf(stderr, "stage=permanent-iat-gateway-unwind failure=%s depth=%u\n", reason,
                 static_cast<unsigned>(GDepth));
    for (USHORT index = 0; index < GDepth; ++index) {
        std::fprintf(stderr, "stage=permanent-iat-gateway-unwind frame[%u]=%p\n", static_cast<unsigned>(index),
                     GFrames[index]);
    }
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] bool frameInCompiledFunction(void *frame, void *function) noexcept
{
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION entry = ::RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(function), &image_base, nullptr);
    if (entry == nullptr) {
        return false;
    }
    const auto address = reinterpret_cast<DWORD64>(frame);
    return address >= image_base + entry->BeginAddress && address < image_base + entry->EndAddress;
}

}  // namespace

int main()
{
    PermanentIatGatewayHandle handle;
    std::string error;
    if (!createPermanentIatGateway(reinterpret_cast<void *>(&originalFive), 1, handle, error)) {
        std::fprintf(stderr, "stage=permanent-iat-gateway-unwind create-failure error=%s\n", error.c_str());
        return 1;
    }
    if (!bindPermanentIatGateway(handle, reinterpret_cast<void *>(&capturingHandler), KDrainTimeoutMs, error)) {
        std::fprintf(stderr, "stage=permanent-iat-gateway-unwind bind-failure error=%s\n", error.c_str());
        return 1;
    }

    auto gateway = reinterpret_cast<FiveArgFn>(handle.gateway);
    const std::uint64_t expected = baseValue(10, 20, 30, 40, 50) + KHandlerBias;
    if (knownCaller(gateway) != expected) {
        fail("handler-result");
    }
    if (GDepth < 2) {
        fail("stack-too-shallow");
    }

    MEMORY_BASIC_INFORMATION gateway_memory{};
    if (::VirtualQuery(handle.gateway, &gateway_memory, sizeof(gateway_memory)) == 0) {
        fail("gateway-virtual-query");
    }
    const auto gateway_begin = reinterpret_cast<std::uintptr_t>(gateway_memory.BaseAddress);
    const auto gateway_end = gateway_begin + gateway_memory.RegionSize;

    bool saw_gateway_frame = false;
    bool saw_known_caller = false;
    for (USHORT index = 0; index < GDepth; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(GFrames[index]);
        if (address != 0 && address < 0x10000U) {
            fail("low-garbage-frame");
        }
        if (address >= gateway_begin && address < gateway_end) {
            saw_gateway_frame = true;
        }
        if (frameInCompiledFunction(GFrames[index], reinterpret_cast<void *>(&knownCaller))) {
            saw_known_caller = true;
        }
    }
    if (saw_gateway_frame) {
        fail("gateway-instrumentation-frame-leaked");
    }
    if (!saw_known_caller) {
        fail("caller-frame-missing-after-gateway");
    }

    if (!detachPermanentIatGateway(handle, KDrainTimeoutMs, error)) {
        std::fprintf(stderr, "stage=permanent-iat-gateway-unwind detach-failure error=%s\n", error.c_str());
        return 1;
    }

    std::fprintf(stderr, "stage=permanent-iat-gateway-unwind pass depth=%u\n", static_cast<unsigned>(GDepth));
    return 0;
}
