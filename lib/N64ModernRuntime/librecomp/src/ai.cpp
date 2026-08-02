#include "recomp.h"
#include <cstdio>
#include <string>
#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>

#define VI_NTSC_CLOCK 48681812

extern "C" void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t freq = ctx->r4;
    // This makes actual audio frequency more accurate to console, but may not be desirable
    //uint32_t dacRate = (uint32_t)(((float)VI_NTSC_CLOCK / freq) + 0.5f);
    //freq = VI_NTSC_CLOCK / dacRate;
    ctx->r2 = freq;
    ultramodern::set_audio_frequency(freq);
}

extern "C" void osAiSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    // libaudio may hand the AI driver an uncached KSEG1 address. The generic
    // host-pointer conversion expects the equivalent cached KSEG0 address;
    // without canonicalizing it, KSEG1 adds 512 MiB and lands in the guard
    // region above the recompiler's committed RDRAM mapping.
    uint32_t audio_buffer = ctx->r4;
    if ((audio_buffer & 0xE0000000U) == 0xA0000000U) {
        audio_buffer = (audio_buffer & 0x1FFFFFFFU) | 0x80000000U;
    }

    constexpr uint32_t rdram_size = 8U * 1024U * 1024U;
    const uint32_t physical_address = audio_buffer & 0x1FFFFFFFU;
    const uint32_t byte_count = static_cast<uint32_t>(ctx->r5);

    // Treat AI DMA arguments as untrusted. A wrapped negative byte count can
    // otherwise turn into billions of samples, stalling the audio thread until
    // it walks through the RDRAM guard region and crashes. Validate both the
    // sample alignment and the complete source range before pointer conversion.
    const bool invalid_range = (byte_count != 0) &&
        ((physical_address >= rdram_size) || (byte_count > (rdram_size - physical_address)));
    if ((byte_count & 1U) != 0 || invalid_range) {
        static uint32_t invalid_audio_dma_warnings = 0;
        if (invalid_audio_dma_warnings < 8) {
            std::fprintf(stderr,
                "[Audio] Ignoring invalid AI DMA: address=0x%08X, bytes=%u\n",
                audio_buffer, byte_count);
            invalid_audio_dma_warnings++;
        }
        ctx->r2 = 0;
        return;
    }

    ultramodern::queue_audio_buffer(rdram, static_cast<int32_t>(audio_buffer), byte_count);
    ctx->r2 = 0;
}

extern "C" void osAiGetLength_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = ultramodern::get_remaining_audio_bytes();
}

extern "C" void osAiGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0x00000000; // Pretend the audio DMAs finish instantly
}
