// Export the analyzed program in one streaming pass; retain the project for later research.
// @category SMBD
import ghidra.app.util.headless.HeadlessScript;
import ghidra.framework.Application;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRangeIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.util.HashSet;
import java.io.BufferedWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public class ExportProgram extends HeadlessScript {
    private static String address(Address value) {
        return String.format("0x%08X", value.getUnsignedOffset());
    }
    private static String text(String value) {
        return value.replace("\\", "\\\\").replace("\t", "\\t")
            .replace("\r", "\\r").replace("\n", "\\n");
    }
    private static BufferedWriter writer(Path directory, String name) throws Exception {
        return Files.newBufferedWriter(directory.resolve(name), StandardCharsets.UTF_8);
    }
    @Override public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 3) throw new IllegalArgumentException("OUTPUT_DIRECTORY EXPECTED_SHA256 NON_CODE_ENTRIES.json");
        if (!currentProgram.getExecutableSHA256().equalsIgnoreCase(args[1]))
            throw new IllegalStateException("Executable SHA-256 differs from supported target");
        if (analysisTimeoutOccurred()) throw new IllegalStateException("Ghidra analysis timed out");
        Path directory = Path.of(args[0]);
        Files.createDirectories(directory);
        var excluded = new HashSet<Long>();
        var evidence = JsonParser.parseString(Files.readString(Path.of(args[2]))).getAsJsonObject();
        if (!evidence.get("executable_sha256").getAsString().equalsIgnoreCase(args[1]))
            throw new IllegalStateException("Code/data evidence belongs to another executable");
        for (var item : evidence.getAsJsonArray("entries"))
            excluded.add(Long.decode(item.getAsJsonObject().get("address").getAsString()));
        for (long location : excluded) {
            var incoming = currentProgram.getReferenceManager().getReferencesTo(toAddr(location));
            while (incoming.hasNext()) {
                var source = getFunctionContaining(incoming.next().getFromAddress());
                if (source != null && !excluded.contains(source.getEntryPoint().getUnsignedOffset()))
                    throw new IllegalStateException("A retained function references rejected code at " + Long.toHexString(location));
            }
        }
        long functions = 0, instructions = 0, symbols = 0, references = 0, computed = 0, orphanInstructions = 0;
        try (var entries = writer(directory, "functions.tsv");
             var ranges = writer(directory, "function-ranges.tsv");
             var seeds = writer(directory, "seeds.txt")) {
            entries.write("entry\tname\tbody_bytes\tthunk\n");
            ranges.write("entry\tstart\tend_inclusive\n");
            seeds.write("# Ghidra analyzed function entries; native decoding independently verifies bytes.\n");
            FunctionIterator iterator = currentProgram.getFunctionManager().getFunctions(true);
            while (iterator.hasNext()) {
                monitor.checkCancelled();
                Function function = iterator.next();
                Address entry = function.getEntryPoint();
                if (function.isExternal() || getInstructionAt(entry) == null) continue;
                if (excluded.contains(entry.getUnsignedOffset())) continue;
                entries.write(address(entry) + "\t" + text(function.getName(true)) + "\t" +
                    function.getBody().getNumAddresses() + "\t" + function.isThunk() + "\n");
                seeds.write(address(entry) + "\n");
                AddressRangeIterator body = function.getBody().getAddressRanges(true);
                while (body.hasNext()) {
                    var range = body.next();
                    ranges.write(address(entry) + "\t" + address(range.getMinAddress()) + "\t" +
                        address(range.getMaxAddress()) + "\n");
                }
                ++functions;
            }
        }
        try (var output = writer(directory, "instructions.tsv");
             var refs = writer(directory, "references.tsv");
             var indirect = writer(directory, "computed-flow.tsv")) {
            output.write("address\tbytes\tinstruction\tflow\n");
            refs.write("from\tto\ttype\toperand\n");
            indirect.write("address\tinstruction\tflow\n");
            InstructionIterator iterator = currentProgram.getListing().getInstructions(true);
            while (iterator.hasNext()) {
                monitor.checkCancelled();
                Instruction instruction = iterator.next();
                Function owner = getFunctionContaining(instruction.getAddress());
                if (owner == null || excluded.contains(owner.getEntryPoint().getUnsignedOffset())) {
                    ++orphanInstructions;
                    continue;
                }
                var bytes = new StringBuilder(instruction.getLength() * 2);
                for (byte value : instruction.getBytes()) bytes.append(String.format("%02X", value & 255));
                String location = address(instruction.getAddress());
                output.write(location + "\t" + bytes + "\t" + text(instruction.toString()) +
                    "\t" + instruction.getFlowType() + "\n");
                for (Reference reference : instruction.getReferencesFrom()) {
                    refs.write(location + "\t" + reference.getToAddress() + "\t" +
                        reference.getReferenceType() + "\t" + reference.getOperandIndex() + "\n");
                    ++references;
                }
                if (instruction.getFlowType().isComputed()) {
                    indirect.write(location + "\t" + text(instruction.toString()) + "\t" +
                        instruction.getFlowType() + "\n");
                    ++computed;
                }
                ++instructions;
            }
        }
        try (var output = writer(directory, "symbols.tsv")) {
            output.write("address\tname\ttype\tsource\n");
            SymbolIterator iterator = currentProgram.getSymbolTable().getAllSymbols(true);
            while (iterator.hasNext()) {
                monitor.checkCancelled();
                Symbol symbol = iterator.next();
                output.write(symbol.getAddress() + "\t" + text(symbol.getName(true)) + "\t" +
                    symbol.getSymbolType() + "\t" + symbol.getSource() + "\n");
                ++symbols;
            }
        }
        try (var output = writer(directory, "memory.tsv")) {
            output.write("name\tstart\tend_inclusive\tbytes\tread\twrite\texecute\tinitialized\n");
            for (MemoryBlock block : currentProgram.getMemory().getBlocks())
                output.write(text(block.getName()) + "\t" + address(block.getStart()) + "\t" +
                    address(block.getEnd()) + "\t" + block.getSize() + "\t" + block.isRead() +
                    "\t" + block.isWrite() + "\t" + block.isExecute() + "\t" + block.isInitialized() + "\n");
        }
        var summary = new JsonObject();
        summary.addProperty("schema_version", 1);
        summary.addProperty("sha256", currentProgram.getExecutableSHA256().toUpperCase());
        summary.addProperty("ghidra_version", Application.getApplicationVersion());
        summary.addProperty("executable_format", currentProgram.getExecutableFormat());
        summary.addProperty("language", currentProgram.getLanguageID().toString());
        summary.addProperty("functions", functions);
        summary.addProperty("instructions", instructions);
        summary.addProperty("orphan_or_rejected_instructions", orphanInstructions);
        summary.addProperty("rejected_function_entries", excluded.size());
        summary.addProperty("symbols", symbols);
        summary.addProperty("references", references);
        summary.addProperty("computed_flow_sites", computed);
        summary.addProperty("analysis_timed_out", false);
        try (var output = writer(directory, "summary.json")) { output.write(summary.toString() + "\n"); }
        println("SMBD_EXPORT " + summary);
    }
}
