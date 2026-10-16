package prism.bridge.nms;

import com.google.gson.JsonElement;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.ServerScoreboard;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.RandomSource;
import net.minecraft.world.Difficulty;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.border.WorldBorder;
import net.minecraft.world.level.dimension.BuiltinDimensionTypes;
import net.minecraft.world.level.gamerules.GameRules;
import net.minecraft.world.flag.FeatureFlags;
import net.minecraft.world.level.storage.ServerLevelData;
import net.minecraft.world.level.storage.WritableLevelData;
import prism.bridge.Main;

import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;

/**
 * The overworld as an NMS ServerLevel: allocated without its constructor,
 * with the fields plugins read set (dimension, registries, random, level
 * data) and block/entity lookups answered by PrismMC. No chunk source: bulk
 * world access stays on the Bukkit API.
 */
public class PrismServerLevel extends ServerLevel {
    private NmsCore core;
    private ServerScoreboard scoreboard;
    private WorldBorder border;
    private GameRules gameRules;

    private PrismServerLevel() {
        super(null, null, null, null, null, null, false, 0L, null, false); // Never run: Unsafe.allocateInstance.
    }

    static PrismServerLevel create(NmsCore core, Object server) {
        PrismServerLevel level = NmsCore.allocate(PrismServerLevel.class);
        level.core = core;
        level.border = new WorldBorder();
        level.gameRules = new GameRules(FeatureFlags.DEFAULT_FLAGS);
        Object levelData = Proxy.newProxyInstance(PrismServerLevel.class.getClassLoader(),
                new Class<?>[] {ServerLevelData.class, WritableLevelData.class}, (proxy, method, args) -> switch (method.getName()) {
                    case "getGameTime", "getDayTime" -> {
                        JsonElement time = Main.rpc.api("world.time");
                        yield time == null || time.isJsonNull() ? 0L : time.getAsLong();
                    }
                    case "getLevelName" -> "world";
                    case "getDifficulty" -> Difficulty.NORMAL;
                    case "getGameType" -> GameType.SURVIVAL;
                    case "getGameRules" -> level.gameRules;
                    case "isInitialized" -> true;
                    case "toString" -> "PrismLevelData";
                    case "hashCode" -> 0;
                    case "equals" -> proxy == args[0];
                    default -> {
                        Class<?> returned = method.getReturnType();
                        yield returned == boolean.class ? false : returned.isPrimitive() && returned != void.class ? 0 : null;
                    }
                });
        NmsCore.setField(Level.class, "thread", level, Thread.currentThread());
        NmsCore.setField(Level.class, "random", level, RandomSource.create());
        NmsCore.setField(Level.class, "dimension", level, Level.OVERWORLD);
        NmsCore.setField(Level.class, "registryAccess", level, NmsCore.registries);
        NmsCore.setField(Level.class, "dimensionTypeRegistration", level,
                NmsCore.vanillaLookup.lookupOrThrow(Registries.DIMENSION_TYPE).getOrThrow(BuiltinDimensionTypes.OVERWORLD));
        NmsCore.setField(Level.class, "levelData", level, levelData);
        NmsCore.setField(ServerLevel.class, "serverLevelData", level, levelData);
        NmsCore.setField(ServerLevel.class, "server", level, server);
        return level;
    }

    @Override
    public BlockState getBlockState(BlockPos pos) {
        JsonElement data = Main.rpc.api("world.blockData", "x", pos.getX(), "y", pos.getY(), "z", pos.getZ());
        int state = data != null && data.isJsonObject() && data.getAsJsonObject().has("state") ? data.getAsJsonObject().get("state").getAsInt() : 0;
        return Block.stateById(state);
    }

    @Override
    public net.minecraft.world.entity.Entity getEntity(int id) {
        return core.entityById(id);
    }

    @Override
    public List<ServerPlayer> players() {
        List<ServerPlayer> list = new ArrayList<>();
        for (NmsPlayer player : core.players.values()) {
            list.add(player.handle);
        }
        return list;
    }

    @Override
    public ServerScoreboard getScoreboard() {
        if (scoreboard == null) {
            scoreboard = new ServerScoreboard((MinecraftServer) core.server());
        }
        return scoreboard;
    }

    private static final java.util.concurrent.atomic.AtomicInteger NEXT_ID = new java.util.concurrent.atomic.AtomicInteger(2_000_000);

    @Override
    public int getNextEntityId() {
        return NEXT_ID.getAndIncrement(); // Vanilla asks the chunk source; PrismMC assigns the real id afterwards.
    }

    @Override
    public WorldBorder getWorldBorder() {
        return border;
    }

    @Override
    public GameRules getGameRules() {
        return gameRules;
    }

    @Override
    public String toString() {
        return "PrismServerLevel[world]";
    }
}
