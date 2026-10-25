#include "pipeline.h"
#include "disc.h"
#include "target.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace smbd {
std::string prepare_disc(const std::filesystem::path& image,
                         const std::filesystem::path& output, bool extract_all) {
    Disc disc(image);
    const auto target_entry = disc.find("default.xbe");
    if (target_entry.size != target::file_size || disc.sha256(target_entry) != target::sha256)
        throw std::runtime_error("Disc executable differs from supported target");
    std::filesystem::create_directories(output);
    std::ofstream index(output / "disc-index.tsv", std::ios::binary);
    if (!index) throw std::runtime_error("Cannot create disc inventory");
    index << "path\tsector\tbytes\tsha256\n";
    std::vector<std::string> directories{""};
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> extensions;
    std::uint64_t files = 0, bytes = 0, extracted = 0, reused = 0;
    for (std::size_t cursor = 0; cursor < directories.size(); ++cursor) {
        if (directories.size() > 65536) throw std::runtime_error("Disc exceeds directory limit");
        const auto directory = directories[cursor];
        for (const auto& entry : disc.list(directory)) {
            const auto path = directory.empty() ? entry.name : directory + '/' + entry.name;
            if (entry.directory()) { directories.push_back(path); continue; }
            if (++files > 1000000) throw std::runtime_error("Disc exceeds file limit");
            bytes += entry.size;
            auto extension = path_text(utf8_path(entry.name).extension());
            std::ranges::transform(extension, extension.begin(), [](unsigned char value) { return char(std::tolower(value)); });
            auto& group = extensions[extension];
            ++group.first; group.second += entry.size;
            const bool executable = directory.empty() && extension == ".xbe";
            const auto digest = disc.sha256(entry);
            index << path << '\t' << entry.sector << '\t' << entry.size << '\t' << digest << '\n';
            if (extract_all || executable) {
                const auto destination = output / utf8_path(path);
                if (std::filesystem::exists(destination)) {
                    File previous(destination);
                    if (previous.size() != entry.size || hash(previous, 0, previous.size()) != digest)
                        throw std::runtime_error("Existing extracted file differs from disc: " + path);
                    ++reused;
                } else {
                    disc.extract(entry, destination);
                    File extracted_file(destination);
                    if (hash(extracted_file, 0, extracted_file.size()) != digest)
                        throw std::runtime_error("Extracted file hash mismatch: " + path);
                    ++extracted;
                }
            }
        }
    }
    index.close();
    if (!index) throw std::runtime_error("Disc inventory write failed");
    Xbe executable(output / "default.xbe");
    if (!executable.supported()) throw std::runtime_error("Disc executable differs from supported target");
    File source(image);
    std::ostringstream report;
    report << "{\"format\":\"smbd-disc-preparation-v1\",\"complete\":true,\"disc_size\":" << source.size()
           << ",\"disc_sha256\":" << json(hash(source, 0, source.size()))
           << ",\"executable_sha256\":" << json(executable.sha256)
           << ",\"files\":" << files << ",\"directories\":" << directories.size()
           << ",\"file_bytes\":" << bytes << ",\"extracted\":" << extracted << ",\"reused\":" << reused
           << ",\"extract_all\":" << (extract_all ? "true" : "false") << ",\"extensions\":[";
    bool first = true;
    for (const auto& [extension, group] : extensions) {
        if (!first) report << ',';
        first = false;
        report << "{\"extension\":" << json(extension) << ",\"files\":" << group.first << ",\"bytes\":" << group.second << '}';
    }
    report << "]}";
    write_text(output / "disc-summary.json", report.str() + '\n', true);
    write_text(output / "executable.json", executable.report() + '\n', true);
    return report.str();
}

std::string verify_analysis(Xbe& image, const std::filesystem::path& instructions) {
    if (!image.supported()) throw std::runtime_error("Instruction comparison requires the supported executable");
    std::ifstream input(instructions);
    std::string line;
    if (!std::getline(input, line) || line != "address\tbytes\tinstruction\tflow")
        throw std::runtime_error("Not a Ghidra instruction index");
    Decoder decoder;
    std::uint64_t count = 0, failures = 0, folded_wait = 0;
    std::uint32_t previous = 0;
    std::ostringstream issues;
    while (std::getline(input, line)) {
        if (++count > 2097152 || line.size() > 4096) throw std::runtime_error("Instruction index exceeds comparison limits");
        const auto separator = line.find('\t');
        const auto end = line.find('\t', separator + 1);
        if (separator == std::string::npos || end == std::string::npos || !line.starts_with("0x"))
            throw std::runtime_error("Malformed instruction index row");
        std::uint32_t address = 0;
        const auto parsed = std::from_chars(line.data() + 2, line.data() + separator, address, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != line.data() + separator || (count > 1 && address <= previous))
            throw std::runtime_error("Invalid or unsorted instruction address");
        previous = address;
        std::string error;
        try {
            const auto instruction = decoder.decode(image, address);
            auto actual = hex_bytes(Bytes(instruction.bytes).first(instruction.decoded.length));
            const auto expected = line.substr(separator + 1, end - separator - 1);
            // Ghidra folds FWAIT into the following x87 instruction; Zydis keeps
            // the two instructions separate. Verify both original encodings.
            if (actual != expected && instruction.decoded.mnemonic == ZYDIS_MNEMONIC_FWAIT &&
                actual == "9B" && expected.starts_with("9B") && expected.size() > 2) {
                const auto next = decoder.decode(image, address + 1);
                const auto combined = actual + hex_bytes(Bytes(next.bytes).first(next.decoded.length));
                if (combined == expected) { actual = combined; ++folded_wait; }
            }
            if (actual != expected) error = "Instruction bytes or length differ";
        } catch (const std::exception& failure) { error = failure.what(); }
        if (!error.empty()) {
            if (failures < 32) {
                if (failures) issues << ',';
                issues << "{\"address\":" << json(hex32(address)) << ",\"reason\":" << json(error) << '}';
            }
            ++failures;
        }
    }
    if (!input.eof()) throw std::runtime_error("Instruction index read failed");
    return "{\"format\":\"smbd-analysis-check-v1\",\"passed\":" + std::string(count && !failures ? "true" : "false") +
        ",\"sha256\":" + json(image.sha256) + ",\"instructions\":" + std::to_string(count) +
        ",\"folded_wait_pairs\":" + std::to_string(folded_wait) +
        ",\"failures\":" + std::to_string(failures) + ",\"first_issues\":[" + issues.str() + "]}";
}
}
