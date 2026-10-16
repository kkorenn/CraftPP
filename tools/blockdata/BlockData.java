// Offline data extractor (like vanilla's --reports generator): dumps block state layout + behaviour flags that no
// report contains (isSolid, canBeReplaced, sturdy faces, collision/occlusion fullness, fluid states) to
// data/blocks.bin. Run by tools/gen_block_data.sh; CraftPP never runs vanilla code at runtime.
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.SupportType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.level.material.Fluids;

public class BlockData {
  static DataOutputStream out;
  static void str(String s) throws IOException { byte[] b = s.getBytes(StandardCharsets.UTF_8); out.writeShort(b.length); out.write(b); }

  @SuppressWarnings({"unchecked", "rawtypes"})
  public static void main(String[] args) throws Exception {
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    out = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(args[0])));
    out.writeInt(0x43424431);  // "CBD1" (big endian)
    List<Block> blocks = new ArrayList<>();
    for (Block b : BuiltInRegistries.BLOCK) blocks.add(b);
    blocks.sort(Comparator.comparingInt(b -> Block.getId(b.defaultBlockState())));
    int nstates = 0;
    for (Block b : blocks) nstates += b.getStateDefinition().getPossibleStates().size();
    out.writeInt(blocks.size());
    out.writeInt(nstates);
    for (Block b : blocks) {
      List<BlockState> states = b.getStateDefinition().getPossibleStates();
      int first = Block.getId(states.get(0));
      str(BuiltInRegistries.BLOCK.getKey(b).getPath());
      out.writeInt(first);
      out.writeInt(Block.getId(b.defaultBlockState()));
      List<Property<?>> props = new ArrayList<>(b.getStateDefinition().getProperties());
      out.writeByte(props.size());
      int[] stride = new int[props.size()];
      int s = 1;
      for (int i = props.size() - 1; i >= 0; i--) { stride[i] = s; s *= props.get(i).getPossibleValues().size(); }
      if (s != states.size()) throw new RuntimeException("state count " + b);
      for (Property p : props) {
        str(p.getName());
        List vals = p.getPossibleValues();
        out.writeByte(vals.size());
        for (Object v : vals) str(p.getName((Comparable) v));
      }
      for (BlockState st : states) {  // verify id = first + sum(index * stride)
        int id = first;
        for (int i = 0; i < props.size(); i++) id += ((List) props.get(i).getPossibleValues()).indexOf(st.getValue((Property) props.get(i))) * stride[i];
        if (id != Block.getId(st)) throw new RuntimeException("layout " + st);
      }
    }
    Direction[] dirs = {Direction.DOWN, Direction.UP, Direction.NORTH, Direction.SOUTH, Direction.WEST, Direction.EAST};
    EmptyBlockGetter g = EmptyBlockGetter.INSTANCE;
    BlockPos o = BlockPos.ZERO;
    for (int id = 0; id < nstates; id++) {
      BlockState st = Block.stateById(id);
      long f = 0;
      if (st.isAir()) f |= 1;
      if (st.isSolid()) f |= 2;
      if (st.canBeReplaced()) f |= 4;
      if (st.isCollisionShapeFullBlock(g, o)) f |= 8;
      if (st.getCollisionShape(g, o).isEmpty()) f |= 16;
      if (st.isSolidRender()) f |= 32;
      if (st.hasBlockEntity()) f |= 64;
      if (st.ignitedByLava()) f |= 128;
      for (int d = 0; d < 6; d++) {
        if (st.isFaceSturdy(g, o, dirs[d], SupportType.FULL)) f |= 1L << (8 + d);
        if (st.isFaceSturdy(g, o, dirs[d], SupportType.CENTER)) f |= 1L << (14 + d);
        if (st.isFaceSturdy(g, o, dirs[d], SupportType.RIGID)) f |= 1L << (20 + d);
        if (Block.isShapeFullBlock(st.getFaceOcclusionShape(dirs[d]))) f |= 1L << (26 + d);
      }
      out.writeInt((int) f);
      FluidState fs = st.getFluidState();
      int type = fs.isEmpty() ? 0 : fs.getType() == Fluids.WATER ? 1 : fs.getType() == Fluids.FLOWING_WATER ? 2 : fs.getType() == Fluids.LAVA ? 3 : 4;
      out.writeByte(type | fs.getAmount() << 3);
    }
    out.close();
    System.out.println("blocks " + blocks.size() + " states " + nstates);
  }
}
