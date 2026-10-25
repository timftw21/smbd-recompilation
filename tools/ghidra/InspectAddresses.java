// Batch evidence for disputed boundaries and platform functions.
// @category SMBD
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import com.google.gson.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public class InspectAddresses extends GhidraScript {
    @Override public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 3) throw new IllegalArgumentException("REQUESTS.json OUTPUT.json EXPECTED_SHA256");
        if (!currentProgram.getExecutableSHA256().equalsIgnoreCase(args[2]))
            throw new IllegalStateException("Executable identity mismatch");
        JsonArray requests = JsonParser.parseString(Files.readString(Path.of(args[0]))).getAsJsonArray();
        if (requests.size() > 256) throw new IllegalArgumentException("At most 256 addresses per research batch");
        JsonArray output = new JsonArray();
        DecompInterface decompiler = new DecompInterface();
        try {
            decompiler.openProgram(currentProgram);
            for (JsonElement element : requests) {
                monitor.checkCancelled();
                JsonObject request = element.getAsJsonObject();
                Address location = toAddr(Long.decode(request.get("address").getAsString()));
                JsonObject row = new JsonObject();
                row.addProperty("address", location.toString());
                var block = currentProgram.getMemory().getBlock(location);
                row.addProperty("block", block == null ? "" : block.getName());
                Function owner = getFunctionContaining(location);
                row.addProperty("function", owner == null ? "" : owner.getEntryPoint().toString());
                row.addProperty("name", owner == null ? "" : owner.getName(true));
                JsonArray refs = new JsonArray();
                var iterator = currentProgram.getReferenceManager().getReferencesTo(location);
                int count = 0;
                while (iterator.hasNext()) {
                    var reference = iterator.next();
                    ++count;
                    if (count > 128) continue;
                    JsonObject ref = new JsonObject();
                    Address source = reference.getFromAddress();
                    Function function = getFunctionContaining(source);
                    ref.addProperty("from", source.toString());
                    ref.addProperty("type", reference.getReferenceType().toString());
                    ref.addProperty("function", function == null ? "" : function.getEntryPoint().toString());
                    ref.addProperty("name", function == null ? "" : function.getName(true));
                    refs.add(ref);
                }
                row.addProperty("reference_count", count);
                row.add("references", refs);
                if (request.has("decompile") && request.get("decompile").getAsBoolean() && owner != null) {
                    var result = decompiler.decompileFunction(owner, 30, monitor);
                    row.addProperty("decompiled", result.decompileCompleted());
                    if (result.decompileCompleted()) row.addProperty("c", result.getDecompiledFunction().getC());
                    else row.addProperty("error", result.getErrorMessage());
                }
                output.add(row);
            }
        } finally { decompiler.dispose(); }
        Files.writeString(Path.of(args[1]), output.toString() + "\n", StandardCharsets.UTF_8);
        println("SMBD_ADDRESS_AUDIT " + output.size() + " addresses");
    }
}
