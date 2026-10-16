package prism.bridge;

/**
 * Java → C++ calls, registered by CraftPP with RegisterNatives when it embeds
 * the JVM (src/plugins/jvm.cpp). JSON payloads cross as UTF-8 byte arrays;
 * hot paths have typed entry points so they never serialize.
 */
public final class Native {
    private Native() {
    }

    /** A generic request ("api" with {m, a}); returns the JSON result, throws IllegalStateException on error. */
    static native byte[] call(byte[] type, byte[] json);

    /** Fire-and-forget notification (listeners, packet, pipeline). */
    static native void notify0(byte[] type, byte[] json);

    static native void log(byte[] level, byte[] logger, byte[] message);

    /** Live position of an online player: fills x, y, z, yaw, pitch; false when offline. */
    static native boolean playerPos(long uuidHi, long uuidLo, double[] out);

    /** Sends a legacy (§-coded) chat line to one player. */
    static native void sendMessage(long uuidHi, long uuidLo, byte[] text);

    /** A clientbound packet (id + body) plugins produced through NMS; CraftPP frames and sends it. */
    public static native void packet(long uuidHi, long uuidLo, byte[] packet);
}
