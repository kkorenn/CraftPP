// Build-time extractor (never run by the server): per-state rotate/mirror results and flags that no report has.
// Output data/block_states.bin: u32 count, then per state id: u16 rotCW90, rotCW180, rotCCW90, mirrorLR, mirrorFB, u32 flags.
// Run: python3 tools/gen_block_dump.py
import java.io.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.*;
import net.minecraft.world.level.block.state.BlockState;

public class BlockDump {
  public static void main(String[] a) throws Exception {
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    int n = Block.BLOCK_STATE_REGISTRY.size();
    try (DataOutputStream o = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(a[0])))) {
      o.writeInt(Integer.reverseBytes(n));
      for (int i = 0; i < n; i++) {
        BlockState s = Block.BLOCK_STATE_REGISTRY.byId(i);
        BlockState[] t = {s.rotate(Rotation.CLOCKWISE_90), s.rotate(Rotation.CLOCKWISE_180), s.rotate(Rotation.COUNTERCLOCKWISE_90),
                          s.mirror(Mirror.LEFT_RIGHT), s.mirror(Mirror.FRONT_BACK)};
        for (BlockState x : t) o.writeShort(Short.reverseBytes((short)Block.BLOCK_STATE_REGISTRY.getId(x)));
        int f = 0;
        if (!s.getBlock().hasDynamicShape() && s.isCollisionShapeFullBlock(EmptyBlockGetter.INSTANCE, BlockPos.ZERO)) f |= 1;
        // bit 2 unused (heightmap motion blocking is the blocks_motion_in_heightmap tag in 26.x)
        if (s.getBlock() instanceof SimpleWaterloggedBlock) f |= 4;
        if (s.getBlock() instanceof LiquidBlockContainer) f |= 8;
        if (s.getFluidState().isSource()) f |= 16;
        if (!s.getFluidState().isEmpty()) f |= 32;
        if (s.isAir()) f |= 64;
        if (s.canBeReplaced()) f |= 128;
        if (s.isSolid()) f |= 256;
        if (s.hasBlockEntity()) f |= 512;
        if (s.isFaceSturdy(EmptyBlockGetter.INSTANCE, BlockPos.ZERO, Direction.UP)) f |= 1024;
        if (s.isSolidRender()) f |= 2048;
        if (s.liquid()) f |= 4096;
        try { if (s.getBlock() instanceof EntityBlock eb && eb.newBlockEntity(BlockPos.ZERO, s) instanceof net.minecraft.world.RandomizableContainer) f |= 8192; } catch (Throwable e) {}
        net.minecraft.world.phys.shapes.VoxelShape col = s.getCollisionShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO);
        for (Direction d : Direction.values()) if (Block.isFaceFull(col, d)) f |= 1 << (16 + d.ordinal());
        if (Block.isShapeFullBlock(s.getShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO))) f |= 1 << 22;
        o.writeInt(Integer.reverseBytes(f));
      }
    }
    System.out.println(n + " states -> " + a[0]);
  }
}
