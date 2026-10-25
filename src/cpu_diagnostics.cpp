#include "pipeline.h"
#include "cpu.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

namespace smbd {
std::string check_cpu(Xbe& image) {
    if (!image.supported()) throw std::runtime_error("CPU diagnostics require the supported executable");
    for (const auto& function : compiled_batch)
        if (function.source_sha256 != image.sha256) throw std::runtime_error("Compiled code belongs to another executable");
    constexpr std::uint32_t data = 0x00C00000, tls = 0x00C10000, stack = 0x00F00000;
    constexpr std::uint32_t sentinel = 0xDEADC0DE;
    if (std::uint64_t(image.base) + image.image_size > data)
        throw std::runtime_error("Diagnostic workspace overlaps the executable image");
    std::vector<std::byte> storage(16 * 1024 * 1024);
    image.load(std::span(storage).first(image.image_size), false);
    Memory memory(image.base, storage);
    const auto require = [](bool condition, std::string_view reason) {
        if (!condition) throw std::runtime_error(std::string(reason));
    };
    const auto run = [&](std::uint32_t address, std::initializer_list<std::uint32_t> arguments) {
        Cpu cpu;
        ExecutionDiagnostic diagnostic;
        diagnostic.remaining = 10000;
        cpu.diagnostic = &diagnostic;
        cpu.fs_base = tls;
        cpu.registers[esp] = stack;
        memory.store<std::uint32_t>(tls, UINT32_MAX);
        memory.store<std::uint32_t>(stack, sentinel);
        unsigned index = 1;
        for (const auto argument : arguments) memory.store<std::uint32_t>(stack + 4 * index++, argument);
        invoke_native(cpu, memory, address);
        require(cpu.eip == sentinel, "Original routine returned to the wrong address");
        require(cpu.registers[esp] == stack + 4 * index, "Original routine left the wrong stack position");
        cpu.diagnostic = nullptr;
        return cpu;
    };
    std::ostringstream results;
    unsigned cases = 0, failures = 0;
    const auto check = [&](std::string_view name, std::uint32_t address, const std::function<void()>& execute) {
        if (cases++) results << ',';
        results << "{\"name\":" << json(name) << ",\"address\":" << json(hex32(address));
        try { execute(); results << ",\"passed\":true}"; }
        catch (const std::exception& error) { ++failures; results << ",\"passed\":false,\"reason\":" << json(error.what()) << '}'; }
    };
    for (const std::string_view text : {"", "Super Monkey Ball Deluxe", "123456789"}) {
        check("original lstrlenA", 0x00308BDE, [&] {
            std::memcpy(memory.access(data, text.size() + 1), text.data(), text.size() + 1);
            const auto cpu = run(0x00308BDE, {data});
            require(cpu.registers[eax] == text.size(), "String length differs");
            require(memory.load<std::uint32_t>(tls) == UINT32_MAX, "SEH chain was not restored");
        });
    }
    for (const std::array<float, 3> scale : {std::array{2.0f, -3.0f, 4.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}) {
        check("original D3DXMatrixScaling", 0x0032E876, [&] {
            std::memset(memory.access(data, 64), 0xA5, 64);
            const auto cpu = run(0x0032E876, {data, std::bit_cast<std::uint32_t>(scale[0]),
                std::bit_cast<std::uint32_t>(scale[1]), std::bit_cast<std::uint32_t>(scale[2])});
            require(cpu.registers[eax] == data, "Scaling returned the wrong output pointer");
            for (unsigned i = 0; i < 16; ++i) {
                const float expected = i == 0 ? scale[0] : i == 5 ? scale[1] : i == 10 ? scale[2] : i == 15 ? 1.0f : 0.0f;
                require(memory.load<std::uint32_t>(data + i * 4) == std::bit_cast<std::uint32_t>(expected),
                    "Scaling matrix differs from independent expected values");
            }
        });
    }
    for (const std::array<float, 3> vector : {std::array{3.0f, 4.0f, 0.0f}, {-2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 5.0f}}) {
        check("original D3DXVec3Normalize", 0x0032EB5A, [&] {
            std::memcpy(memory.access(data + 128, 12), vector.data(), 12);
            const auto cpu = run(0x0032EB5A, {data, data + 128});
            require(cpu.registers[eax] == data, "Normalization returned the wrong output pointer");
            const auto length = std::sqrt(double(vector[0]) * vector[0] + double(vector[1]) * vector[1] + double(vector[2]) * vector[2]);
            for (unsigned i = 0; i < 3; ++i)
                require(std::abs(std::bit_cast<float>(memory.load<std::uint32_t>(data + i * 4)) - vector[i] / length) < 0.000001,
                    "Normalized vector differs from independently calculated values");
        });
    }
    check("original D3DXMatrixMultiply", 0x0032EBD6, [&] {
        const std::array<float, 16> left{1,2,3,4, 5,6,7,8, 2,0,-2,1, 0,0,0,1};
        const std::array<float, 16> right{2,0,0,0, 0,3,0,0, 0,0,4,0, 1,2,3,1};
        std::memcpy(memory.access(data + 128, 64), left.data(), 64);
        std::memcpy(memory.access(data + 256, 64), right.data(), 64);
        const auto cpu = run(0x0032EBD6, {data, data + 128, data + 256});
        require(cpu.registers[eax] == data, "Multiplication returned the wrong output pointer");
        for (unsigned row = 0; row < 4; ++row) for (unsigned column = 0; column < 4; ++column) {
            float expected = 0;
            for (unsigned k = 0; k < 4; ++k) expected += left[row * 4 + k] * right[k * 4 + column];
            require(std::bit_cast<float>(memory.load<std::uint32_t>(data + (row * 4 + column) * 4)) == expected,
                "Matrix product differs from independently calculated values");
        }
    });
    return "{\"format\":\"smbd-cpu-check-v1\",\"passed\":" + std::string(failures ? "false" : "true") +
        ",\"sha256\":" + json(image.sha256) + ",\"compiled_functions\":" + std::to_string(compiled_batch.size()) +
        ",\"cases\":" + std::to_string(cases) + ",\"failures\":" + std::to_string(failures) +
        ",\"kernel_bindings\":0,\"platform_bindings\":0,\"results\":[" + results.str() + "]}";
}
}
