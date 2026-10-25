#include "batch.h"
#include "disc.h"
#include "pipeline.h"
#include <charconv>
#include <chrono>
#include <format>
#include <iostream>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
std::uint32_t number(std::string_view text) {
    auto value = smbd::utf8(text);
    int base = 10;
    if (value.starts_with("0x") || value.starts_with("0X")) { base = 16; value.erase(0, 2); }
    std::uint32_t result;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), result, base);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size())
        throw std::runtime_error("Invalid unsigned number: " + value);
    return result;
}
constexpr auto usage =
    "Super Monkey Ball Deluxe native tools (C++23)\n"
    "  smbd-tool prepare ISO OUTPUT_DIRECTORY [--extract-all]\n"
    "  smbd-tool list ISO [DIRECTORY]\n"
    "  smbd-tool extract ISO DISC_PATH OUTPUT\n"
    "  smbd-tool inspect|verify|check-image XBE\n"
    "  smbd-tool verify-analysis XBE INSTRUCTIONS.tsv\n"
    "  smbd-tool data|decode|analyse XBE ADDRESS LIMIT\n"
    "  smbd-tool recompile XBE ADDRESS LIMIT OUTPUT.cpp [--replace]\n"
    "  smbd-tool batch XBE SEEDS.txt FUNCTION_LIMIT INSTRUCTION_LIMIT OUTPUT.cpp [--replace] [--allow-partial] [--shards COUNT] [--report REPORT.json]\n"
#ifdef SMBD_HAS_BATCH
    "  smbd-tool check-cpu XBE\n"
#endif
    "Addresses accept decimal or hexadecimal. Decode/data: 64 KiB; analysis: 8192 instructions.\n"
    "Batch: 65536 functions, 2097152 total instructions, 256 MiB source, 1..512 shards.\n"
    "Partial batches return 3 unless --allow-partial is explicitly supplied.\n";
}

int tool_main(int argc, char** argv) {
    try {
        std::vector<std::filesystem::path> paths;
        for (int i = 0; i < argc; ++i) paths.push_back(smbd::utf8_path(argv[i]));
        if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
            std::cout << usage;
            return 0;
        }
        const auto command = std::string_view(argv[1]);
        const auto started = std::chrono::steady_clock::now();
        int result = 0;
        std::string output;
        if (command == "prepare" && (argc == 4 || (argc == 5 && std::string_view(argv[4]) == "--extract-all"))) {
            output = smbd::prepare_disc(paths[2], paths[3], argc == 5);
        } else if (command == "verify-analysis" && argc == 4) {
            smbd::Xbe image(paths[2]);
            output = smbd::verify_analysis(image, paths[3]);
            if (output.find("\"passed\":false") != std::string::npos) result = 3;
#ifdef SMBD_HAS_BATCH
        } else if (command == "check-cpu" && argc == 3) {
            smbd::Xbe image(paths[2]);
            output = smbd::check_cpu(image);
            if (output.find("\"passed\":false") != std::string::npos) result = 3;
#endif
        } else if (command == "list" && (argc == 3 || argc == 4)) {
            smbd::Disc disc(paths[2]);
            const auto entries = disc.list(argc == 4 ? smbd::utf8(argv[3]) : "");
            output = "{\"format\":\"smbd-disc-v1\",\"entries\":[";
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const auto& entry = entries[i];
                if (i) output += ',';
                output += "{\"name\":" + smbd::json(entry.name) + ",\"size\":" + std::to_string(entry.size) +
                          ",\"sector\":" + std::to_string(entry.sector) + ",\"directory\":" +
                          (entry.directory() ? "true" : "false") + '}';
            }
            output += "]}";
        } else if (command == "extract" && argc == 5) {
            smbd::Disc disc(paths[2]);
            const auto path = smbd::utf8(argv[3]);
            const auto entry = disc.find(path);
            disc.extract(path, paths[4]);
            output = "{\"format\":\"smbd-extract-v1\",\"file\":" + smbd::json(path) +
                     ",\"bytes\":" + std::to_string(entry.size) + '}';
        } else if ((command == "inspect" || command == "verify" || command == "check-image") && argc == 3) {
            smbd::Xbe image(paths[2]);
            output = command == "check-image" ? smbd::check_image(image) : image.report();
            if (command == "verify" && !image.supported()) result = 2;
        } else if (command == "batch" && argc >= 7 && argc <= 14) {
            bool replace = false, allow_partial = false;
            std::uint32_t shard_count=0;
            std::filesystem::path report_path;
            for (int i = 7; i < argc; ++i) {
                const std::string_view option(argv[i]);
                if (option == "--replace" && !replace) replace = true;
                else if (option == "--allow-partial" && !allow_partial) allow_partial = true;
                else if (option == "--report" && report_path.empty() && i + 1 < argc) report_path = paths[++i];
                else if (option == "--shards" && !shard_count && i + 1 < argc) {
                    shard_count=number(argv[++i]);
                    if(!shard_count || shard_count>512) throw std::runtime_error("Shard count must be 1..512");
                }
                else throw std::runtime_error("Unknown or duplicate batch option");
            }
            smbd::Xbe image(paths[2]);
            if (!image.supported()) { std::cout << image.report() << '\n'; return 2; }
            smbd::File seed_file(paths[3]);
            if (seed_file.size() > 1024 * 1024) throw std::runtime_error("Seed file exceeds 1 MiB");
            const auto seed_bytes = seed_file.read(0, static_cast<std::size_t>(seed_file.size()));
            std::istringstream lines(std::string(reinterpret_cast<const char*>(seed_bytes.data()), seed_bytes.size()));
            std::vector<std::uint32_t> seeds;
            std::set<std::uint32_t> unique_seeds;
            const auto seed=[&](std::uint32_t address) {
                if(unique_seeds.insert(address).second) seeds.push_back(address);
                if(seeds.size()>65536) throw std::runtime_error("Seed file exceeds 65536 unique addresses");
            };
            std::string line;
            while (std::getline(lines, line)) {
                line = line.substr(0, line.find('#'));
                std::istringstream tokens(line);
                std::string value, extra;
                if (!(tokens >> value)) continue;
                if(value=="table") {
                    std::string first,last;
                    if(!(tokens>>first>>last) || (tokens>>extra)) throw std::runtime_error("Table seed needs START END addresses");
                    const auto start=number(first);
                    const auto end=number(last);
                    if(end<=start || (start&3U) || (end&3U) || end-start>128*1024)
                        throw std::runtime_error("Pointer table must be aligned and at most 128 KiB");
                    for(auto cursor=start;cursor<end;) {
                        const auto size=std::min<std::uint32_t>(4096,end-cursor);
                        const auto data=image.data(cursor,size);
                        for(std::uint32_t offset=0;offset<size;offset+=4) {
                            const auto target=smbd::u32(data,offset);
                            if(target && target!=UINT32_MAX) seed(target);
                        }
                        cursor+=size;
                    }
                    continue;
                }
                if (tokens >> extra) throw std::runtime_error("Expected one address per seed line");
                seed(value == "entry" ? image.entry : number(value));
            }
            const std::filesystem::path destination(paths[6]);
            auto assembly_path=destination;
            assembly_path+=".asm";
            auto header_path=destination; header_path+=".h";
            std::vector<std::filesystem::path> outputs{destination,assembly_path};
            std::vector<std::filesystem::path> shard_paths;
            if(shard_count) {
                outputs.push_back(header_path);
                for(std::uint32_t i=0;i<shard_count;++i) {
                    auto path=destination; path+=std::format(".{:02}.cpp",i);
                    shard_paths.push_back(path); outputs.push_back(path);
                }
            }
            if (destination.extension() != ".cpp") throw std::runtime_error("Native output must be a .cpp file");
            for(const auto& path : outputs) for(const auto input : {paths[2],paths[3]})
                if(std::filesystem::exists(path) && std::filesystem::equivalent(path,input))
                    throw std::runtime_error("Native output cannot replace a batch input");
            if (!report_path.empty()) {
                if (report_path.extension() != ".json") throw std::runtime_error("Batch report must be a .json file");
                auto protected_paths=outputs; protected_paths.emplace_back(paths[2]); protected_paths.emplace_back(paths[3]);
                for (const auto& input : protected_paths)
                    if (std::filesystem::absolute(report_path).lexically_normal() == std::filesystem::absolute(input).lexically_normal() ||
                        (std::filesystem::exists(report_path) && std::filesystem::exists(input) && std::filesystem::equivalent(report_path, input)))
                        throw std::runtime_error("Batch report cannot replace another input or output");
            }
            auto batch = smbd::recompile_batch(image, seeds, number(argv[4]), number(argv[5]),shard_count,
                shard_count?smbd::path_text(header_path.filename()):std::string{});
            const bool publish = !batch.source.empty() && (batch.complete || allow_partial);
            if (publish) {
                for(const auto& path : outputs) if(!replace && std::filesystem::exists(path))
                    throw std::runtime_error("Native output already exists; use --replace explicitly");
                smbd::write_text(assembly_path,batch.assembly,replace);
                if(shard_count) {
                    smbd::write_text(header_path,batch.header,replace);
                    for(std::uint32_t i=0;i<shard_count;++i) smbd::write_text(shard_paths[i],batch.shards[i],replace);
                }
                smbd::write_text(destination, batch.source, replace);
            }
            batch.report.pop_back();
            std::size_t source_bytes=batch.source.size()+batch.assembly.size()+(shard_count?batch.header.size():0);
            for(const auto& piece : batch.shards) source_bytes+=piece.size();
            output = batch.report + ",\"source_bytes\":" + std::to_string(source_bytes) +
                     ",\"source_written\":" + (publish ? "true" : "false") + '}';
            if (!report_path.empty()) {
                output.pop_back();
                const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                smbd::write_text(report_path, output + ",\"elapsed_ms\":" + std::to_string(elapsed) + '}'+ '\n', replace);
                output = "{\"format\":\"smbd-batch-summary-v1\",\"complete\":" + std::string(batch.complete ? "true" : "false") +
                    ",\"functions_discovered\":" + std::to_string(batch.discovered) + ",\"functions_compiled\":" + std::to_string(batch.compiled) +
                    ",\"instructions_discovered\":" + std::to_string(batch.instructions) + ",\"report\":" + smbd::json(smbd::path_text(report_path)) + '}';
            }
            if (!batch.complete && !allow_partial) result = 3;
            if (batch.source.empty()) result = 3;
        } else if (((command == "decode" || command == "analyse" || command == "data") && argc == 5) ||
                   (command == "recompile" && (argc == 6 || (argc == 7 && std::string_view(argv[6]) == "--replace")))) {
            smbd::Xbe image(paths[2]);
            // Applying address-specific research requires the exact audited image.
            if (!image.supported()) {
                std::cout << image.report() << '\n';
                return 2;
            }
            const auto address = number(argv[3]);
            const auto limit = number(argv[4]);
            if (command == "recompile") {
                const std::filesystem::path destination(paths[5]);
                if (destination.extension() != ".cpp") throw std::runtime_error("Native output must be a .cpp file");
                if (std::filesystem::exists(destination) && std::filesystem::equivalent(destination, paths[2]))
                    throw std::runtime_error("Native output cannot replace the input executable");
                const auto source = smbd::recompile(image, address, limit);
                smbd::write_text(destination, source, argc == 7);
                output = "{\"format\":\"smbd-native-v1\",\"sha256\":" + smbd::json(image.sha256) +
                         ",\"entry\":" + smbd::json(smbd::hex32(address)) + ",\"source_bytes\":" + std::to_string(source.size()) + '}';
            } else if (command == "data") output = "{\"format\":\"smbd-data-v1\",\"sha256\":" + smbd::json(image.sha256) +
                ",\"address\":" + smbd::json(smbd::hex32(address)) + ",\"bytes\":" + smbd::json(smbd::hex_bytes(image.data(address,limit))) + '}';
            else output = command == "decode" ? smbd::disassemble(image, address, limit) : smbd::analyse(image, address, limit);
        } else {
            throw std::runtime_error("Unknown command or incorrect arguments; use --help");
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        output.pop_back();
        std::cout << output << ",\"elapsed_ms\":" << elapsed << "}\n";
        return result;
    } catch (const std::exception& error) {
        std::cout << "{\"error\":" << smbd::json(error.what()) << "}\n";
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> values;values.reserve(argc);
    for(int i=0;i<argc;++i) values.push_back(smbd::utf8(argv[i]));
    std::vector<char*> arguments;arguments.reserve(argc);
    for(auto& value:values) arguments.push_back(value.data());
    return tool_main(argc,arguments.data());
}
#else
int main(int argc,char** argv) {return tool_main(argc,argv);}
#endif
