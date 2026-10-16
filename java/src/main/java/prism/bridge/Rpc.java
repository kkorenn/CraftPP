package prism.bridge;

import com.google.gson.Gson;
import com.google.gson.JsonElement;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The bridge's link to the host server. Originally NDJSON over a sidecar
 * process' stdio (PrismMC); in CraftPP it is in-process JNI: every call is a
 * direct native call on the calling thread, so there are no ids, pipes, or
 * reader threads, and an API call made while CraftPP waits on an event simply
 * runs on the waiting chain.
 */
public final class Rpc {
    public static final Gson GSON = new Gson();

    private volatile boolean closed;
    /** Requests from CraftPP that a handler deferred to the server thread, by synthetic id. */
    final Map<Long, CompletableFuture<JsonElement>> waiting = new ConcurrentHashMap<>();

    public boolean isClosed() {
        return closed;
    }

    void close() {
        closed = true;
    }

    static byte[] utf8(String text) {
        return text == null ? new byte[0] : text.getBytes(StandardCharsets.UTF_8);
    }

    /** Blocking request to CraftPP. */
    public JsonElement call(String type, JsonObject payload) {
        byte[] result = Native.call(utf8(type), utf8(GSON.toJson(payload == null ? new JsonObject() : payload)));
        if (result == null || result.length == 0) {
            return JsonNull.INSTANCE;
        }
        return JsonParser.parseString(new String(result, StandardCharsets.UTF_8));
    }

    /** Convenience: an API call with a small payload. */
    public JsonElement api(String method, Object... keyValues) {
        JsonObject args = new JsonObject();
        for (int i = 0; i + 1 < keyValues.length; i += 2) {
            Object value = keyValues[i + 1];
            String key = String.valueOf(keyValues[i]);
            if (value == null) {
                args.add(key, JsonNull.INSTANCE);
            } else if (value instanceof JsonElement element) {
                args.add(key, element);
            } else if (value instanceof Number number) {
                args.addProperty(key, number);
            } else if (value instanceof Boolean bool) {
                args.addProperty(key, bool);
            } else {
                args.addProperty(key, value.toString());
            }
        }
        JsonObject payload = new JsonObject();
        payload.addProperty("m", method);
        payload.add("a", args);
        return call("api", payload);
    }

    /** Fire-and-forget notification (no reply expected). */
    public void notify(String type, JsonObject payload) {
        if (!closed) {
            Native.notify0(utf8(type), utf8(GSON.toJson(payload == null ? new JsonObject() : payload)));
        }
    }

    public void log(String level, String logger, String text) {
        Native.log(utf8(level), utf8(logger), utf8(text));
    }

    /** Sentinel a handler returns when it will call {@link #replyLater} itself. */
    public static final JsonElement DEFERRED = new JsonObject();

    public void replyLater(long id, JsonElement result, Throwable error) {
        CompletableFuture<JsonElement> future = waiting.get(id);
        if (future == null) {
            return;
        }
        if (error != null) {
            future.completeExceptionally(error);
        } else {
            future.complete(result);
        }
    }

    static String describe(Throwable t) {
        String message = t.getMessage();
        return t.getClass().getSimpleName() + (message == null ? "" : ": " + message);
    }
}
