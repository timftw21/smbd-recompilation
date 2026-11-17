#pragma once
#include "file.h"
#include <array>

namespace smbd {
// Real-ESRGAN realesr-animevideov3 (release v0.2.5.0, BSD-3-Clause), shipped
// unmodified in resources/upscaler. SRVGGNetCompact: a 3->64 convolution, 16
// 64->64 convolutions, each followed by per-channel PReLU, then a 64->48
// convolution, x4 pixel shuffle and the nearest-upsampled input added back.
// Every convolution is 3x3 with one pixel of zero padding.
inline constexpr unsigned upscaler_layers=18,upscaler_scale=4,upscaler_features=64;
inline constexpr std::uint64_t upscaler_model_bytes=2504012;
inline constexpr std::string_view upscaler_model_sha256="B8A8376811077954D82CA3FCF476F1AC3DA3E8A68A4F4D71363008000A18B75D";
struct UpscalerLayer {
    unsigned inputs,outputs;
    std::size_t weights,bias,slope; // Float offsets; slope is SIZE_MAX for the final linear layer.
};
// Tensors in model order: weight[out][in][3][3], bias[out], then PReLU slope[out].
std::array<UpscalerLayer,upscaler_layers> upscaler_layout();
std::size_t upscaler_model_floats();
// Verifies the checkpoint's size and SHA-256 and reads its stored tensor entries.
std::vector<float> read_upscaler_model(const std::filesystem::path&);
}
