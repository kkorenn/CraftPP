package prism.bridge;

import io.papermc.paper.ServerBuildInfo;
import net.kyori.adventure.key.Key;

import java.time.Instant;
import java.util.Optional;
import java.util.OptionalInt;

/** {@code io.papermc.paper.ServerBuildInfo}: what /version and Bukkit.getVersionMessage report. */
public final class BuildInfoImpl implements ServerBuildInfo {
    static volatile String minecraftVersion = "26.3";
    static volatile String version = "26.3-CraftPP";
    private static final Key BRAND = Key.key("prismmc", "prismmc");

    @Override
    public Key brandId() {
        return BRAND;
    }

    @Override
    public boolean isBrandCompatible(Key brandId) {
        return brandId.equals(BRAND) || brandId.equals(BRAND_PAPER_ID);
    }

    @Override
    public String brandName() {
        return "CraftPP";
    }

    @Override
    public String minecraftVersionId() {
        return minecraftVersion;
    }

    @Override
    public String minecraftVersionName() {
        return minecraftVersion;
    }

    @Override
    public OptionalInt buildNumber() {
        return OptionalInt.empty();
    }

    @Override
    public Instant buildTime() {
        return Instant.EPOCH;
    }

    @Override
    public Optional<String> gitBranch() {
        return Optional.empty();
    }

    @Override
    public Optional<String> gitCommit() {
        return Optional.empty();
    }

    @Override
    public String asString(StringRepresentation representation) {
        return representation == StringRepresentation.VERSION_SIMPLE ? version : version + " (MC: " + minecraftVersion + ")";
    }
}
