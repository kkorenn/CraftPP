// Build-time data extraction for tools/gen_level_data.py: loads the vanilla 26.3 server jar and prints per-state
// facts that no data report contains (collision boxes, legacy solid/liquid flags, face sturdiness, random ticking,
// light, fluid state) plus per-block class chains and fire odds, as one JSON document. Never used at runtime.
import java.lang.reflect.Method;
import java.util.*;

import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.FireBlock;
import net.minecraft.world.level.block.SupportType;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FlowingFluid;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.AABB;

public class BlockDump {
    static String num(double d) {
        if (d == Math.rint(d) && Math.abs(d) < 1e15) return Long.toString((long) d);
        return Double.toString(d);
    }

    static class One implements BlockGetter {
        BlockState s;
        One(BlockState s) { this.s = s; }
        public BlockEntity getBlockEntity(BlockPos p) { return null; }
        public BlockState getBlockState(BlockPos p) { return p.equals(BlockPos.ZERO) ? s : Blocks.AIR.defaultBlockState(); }
        public FluidState getFluidState(BlockPos p) { return getBlockState(p).getFluidState(); }
        public int getHeight() { return 384; }
        public int getMinY() { return -64; }
    }

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        StringBuilder out = new StringBuilder("{");
        Map<String, Integer> shapeIds = new LinkedHashMap<>();
        List<String> shapes = new ArrayList<>();
        StringBuilder st = new StringBuilder();
        int n = Block.BLOCK_STATE_REGISTRY.size();
        for (int i = 0; i < n; i++) {
            BlockState s = Block.BLOCK_STATE_REGISTRY.byId(i);
            One lvl = new One(s);
            StringBuilder sh = new StringBuilder("[");
            for (AABB b : s.getCollisionShape(lvl, BlockPos.ZERO).toAabbs()) {
                if (sh.length() > 1) sh.append(',');
                sh.append(num(b.minX)).append(',').append(num(b.minY)).append(',').append(num(b.minZ)).append(',')
                  .append(num(b.maxX)).append(',').append(num(b.maxY)).append(',').append(num(b.maxZ));
            }
            sh.append(']');
            Integer id = shapeIds.get(sh.toString());
            if (id == null) { id = shapes.size(); shapeIds.put(sh.toString(), id); shapes.add(sh.toString()); }
            long f = 0;
            if (s.isAir()) f |= 1;
            if (s.canBeReplaced()) f |= 2;
            if (s.isSolid()) f |= 4;
            if (s.liquid()) f |= 8;
            if (s.isRandomlyTicking()) f |= 16;
            if (s.ignitedByLava()) f |= 32;
            if (s.isCollisionShapeFullBlock(lvl, BlockPos.ZERO)) f |= 64;
            if (s.hasBlockEntity()) f |= 128;
            if (s.isSolidRender()) f |= 256;
            if (s.isRedstoneConductor(lvl, BlockPos.ZERO)) f |= 512;
            if (s.isSignalSource()) f |= 1024;
            if (s.propagatesSkylightDown()) f |= 2048;
            if (s.hasAnalogOutputSignal()) f |= 4096;
            if (s.isSuffocating(lvl, BlockPos.ZERO)) f |= 8192;
            if (net.minecraft.world.level.lighting.LightEngine.getLightDampeningInto(Blocks.GRASS_BLOCK.defaultBlockState(), s, Direction.UP, s.getLightDampening()) < 15) f |= 16384;
            if (s.canOcclude()) f |= 32768;
            int k = 0;
            for (SupportType t : new SupportType[]{SupportType.FULL, SupportType.CENTER, SupportType.RIGID})
                for (Direction d : Direction.values()) {
                    if (s.isFaceSturdy(lvl, BlockPos.ZERO, d, t)) f |= 1L << (16 + k);
                    k++;
                }
            f |= (long) s.getLightDampening() << 34;
            f |= (long) s.getLightEmission() << 38;
            FluidState fs = s.getFluidState();
            int fl = 0;
            if (!fs.isEmpty()) {
                boolean water = fs.getType() == net.minecraft.world.level.material.Fluids.WATER || fs.getType() == net.minecraft.world.level.material.Fluids.FLOWING_WATER;
                fl = (water ? 1 : 3) + (fs.isSource() ? 1 : 0);
                fl |= fs.getAmount() << 3;
                if (fs.getValue(FlowingFluid.FALLING)) fl |= 128;
            }
            f |= (long) fl << 42;
            if (i > 0) st.append(',');
            st.append('[').append(id).append(',').append(f).append(']');
        }
        out.append("\"shapes\":[").append(String.join(",", shapes)).append("],");
        out.append("\"states\":[").append(st).append("],");

        FireBlock fire = (FireBlock) Blocks.FIRE;
        Method burn = FireBlock.class.getDeclaredMethod("getBurnOdds", BlockState.class);
        Method ignite = FireBlock.class.getDeclaredMethod("getIgniteOdds", BlockState.class);
        burn.setAccessible(true);
        ignite.setAccessible(true);
        StringBuilder bl = new StringBuilder();
        for (Block b : BuiltInRegistries.BLOCK) {
            if (bl.length() > 0) bl.append(',');
            StringBuilder cls = new StringBuilder();
            for (Class<?> c = b.getClass(); c != null && c != Block.class && c != Object.class; c = c.getSuperclass()) {
                if (cls.length() > 0) cls.append('<');
                cls.append(c.getSimpleName());
            }
            BlockState d = b.defaultBlockState();
            bl.append("[\"").append(BuiltInRegistries.BLOCK.getKey(b).getPath()).append("\",\"").append(cls).append("\",")
              .append(burn.invoke(fire, d)).append(',').append(ignite.invoke(fire, d)).append(']');
        }
        out.append("\"blocks\":[").append(bl).append("]}");
        java.nio.file.Files.writeString(java.nio.file.Path.of(args[0]), out);
    }
}
