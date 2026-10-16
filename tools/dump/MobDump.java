// Build-time data extraction for tools/gen_mob_data.py: loads the vanilla 26.3 server jar's registries and prints
// facts no data report contains (collision shapes, block physics, path types, entity types, default attributes,
// synched entity data ids) as one JSON document on stdout. Never used at runtime.
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.*;
import java.util.jar.JarFile;

import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.syncher.EntityDataAccessor;
import net.minecraft.network.syncher.EntityDataSerializers;
import net.minecraft.server.Bootstrap;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.EntityTypes;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.ai.attributes.Attribute;
import net.minecraft.world.entity.ai.attributes.AttributeSupplier;
import net.minecraft.world.entity.ai.attributes.DefaultAttributes;
import net.minecraft.world.entity.ai.attributes.RangedAttribute;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import net.minecraft.world.phys.AABB;

public class MobDump {
    static StringBuilder out = new StringBuilder();

    static String q(String s) { return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\""; }
    static String num(double d) {
        if (d == Math.rint(d) && Math.abs(d) < 1e15) return Long.toString((long) d);
        return Double.toString(d);
    }

    // A level holding one block state at the origin, air elsewhere.
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
        out.append("{");

        // ---------- block states ----------
        Method pathFromState = WalkNodeEvaluator.class.getDeclaredMethod("getPathTypeFromState", BlockGetter.class, BlockPos.class);
        pathFromState.setAccessible(true);
        Map<String, Integer> shapeIds = new LinkedHashMap<>();
        List<String> shapes = new ArrayList<>();
        StringBuilder stateShape = new StringBuilder(), stateFlags = new StringBuilder(), statePath = new StringBuilder(),
                stateFluid = new StringBuilder();
        int n = Block.BLOCK_STATE_REGISTRY.size();
        EntityType<?>[] spawnTypes = {EntityTypes.ZOMBIE, EntityTypes.STRIDER, EntityTypes.POLAR_BEAR, EntityTypes.OCELOT, EntityTypes.PARROT};
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
            String key = sh.toString();
            Integer id = shapeIds.get(key);
            if (id == null) { id = shapes.size(); shapeIds.put(key, id); shapes.add(key); }
            if (i > 0) { stateShape.append(','); stateFlags.append(','); statePath.append(','); stateFluid.append(','); }
            stateShape.append(id);
            int f = 0;
            if (s.isCollisionShapeFullBlock(lvl, BlockPos.ZERO)) f |= 1;
            if (s.isRedstoneConductor(lvl, BlockPos.ZERO)) f |= 2;
            if (s.isSignalSource()) f |= 4;
            for (int k = 0; k < spawnTypes.length; k++) if (s.isValidSpawn(lvl, BlockPos.ZERO, spawnTypes[k])) f |= 8 << k;
            if (s.canBeReplaced()) f |= 256;
            if (s.isAir()) f |= 1024;
            if (s.isSolid()) f |= 2048;
            if (s.isSuffocating(lvl, BlockPos.ZERO)) f |= 4096;
            if (s.isSolidRender()) f |= 8192;
            f |= s.getLightEmission() << 16;
            for (Direction d : Direction.values()) if (s.isFaceSturdy(lvl, BlockPos.ZERO, d)) f |= 1 << (20 + d.ordinal());
            stateFlags.append(f);
            statePath.append(((PathType) pathFromState.invoke(null, lvl, BlockPos.ZERO)).ordinal());
            FluidState fs = s.getFluidState();
            int fl = 0;
            if (!fs.isEmpty()) {
                fl = fs.getAmount() | (fs.is(FluidTags.WATER) ? 16 : fs.is(FluidTags.LAVA) ? 32 : 48);
                if (fs.isSource()) fl |= 128;
                if (fs.getValue(net.minecraft.world.level.material.FlowingFluid.FALLING)) fl |= 64;
            }
            stateFluid.append(fl);
        }
        out.append("\"shapes\":[").append(String.join(",", shapes)).append("],");
        out.append("\"state_shape\":[").append(stateShape).append("],");
        out.append("\"state_flags\":[").append(stateFlags).append("],");
        out.append("\"state_path\":[").append(statePath).append("],");
        out.append("\"state_fluid\":[").append(stateFluid).append("],");
        StringBuilder pt = new StringBuilder();
        for (PathType p : PathType.values()) {
            if (pt.length() > 0) pt.append(',');
            pt.append("[").append(q(p.name())).append(',').append(num(p.getMalus())).append("]");
        }
        out.append("\"path_types\":[").append(pt).append("],");

        // ---------- blocks: physics ----------
        StringBuilder bl = new StringBuilder();
        for (Block b : BuiltInRegistries.BLOCK) {
            if (bl.length() > 0) bl.append(',');
            bl.append("[").append(q(BuiltInRegistries.BLOCK.getKey(b).getPath())).append(',').append(num(b.getFriction())).append(',')
              .append(num(b.getSpeedFactor())).append(',').append(num(b.getJumpFactor())).append(',')
              .append(num(b.getExplosionResistance())).append("]");
        }
        out.append("\"blocks\":[").append(bl).append("],");

        // ---------- attributes ----------
        StringBuilder at = new StringBuilder();
        for (Attribute a : BuiltInRegistries.ATTRIBUTE) {
            if (at.length() > 0) at.append(',');
            double lo = a instanceof RangedAttribute r ? r.getMinValue() : -Double.MAX_VALUE;
            double hi = a instanceof RangedAttribute r ? r.getMaxValue() : Double.MAX_VALUE;
            at.append("[").append(q(BuiltInRegistries.ATTRIBUTE.getKey(a).getPath())).append(',').append(num(a.getDefaultValue()))
              .append(',').append(num(lo)).append(',').append(num(hi)).append(',').append(a.isClientSyncable()).append("]");
        }
        out.append("\"attributes\":[").append(at).append("],");

        // ---------- entity types ----------
        Field immune = EntityType.class.getDeclaredField("immuneTo");
        immune.setAccessible(true);
        StringBuilder et = new StringBuilder();
        for (EntityType<?> t : BuiltInRegistries.ENTITY_TYPE) {
            if (et.length() > 0) et.append(',');
            var d = t.getDimensions();
            et.append("{\"id\":").append(q(BuiltInRegistries.ENTITY_TYPE.getKey(t).getPath()))
              .append(",\"w\":").append(num(d.width())).append(",\"h\":").append(num(d.height()))
              .append(",\"eye\":").append(num(d.eyeHeight())).append(",\"fixed\":").append(d.fixed())
              .append(",\"cat\":").append(q(t.getCategory().getName()))
              .append(",\"range\":").append(t.clientTrackingRange()).append(",\"interval\":").append(t.updateInterval())
              .append(",\"deltas\":").append(t.trackDeltas())
              .append(",\"fire_immune\":").append(t.fireImmune()).append(",\"summon\":").append(t.canSummon())
              .append(",\"serialize\":").append(t.canSerialize()).append(",\"far\":").append(t.canSpawnFarFromPlayer())
              .append(",\"peaceful\":").append(t.isAllowedInPeaceful())
              .append(",\"class\":").append(q(t.getBaseClass().getSimpleName()));
            Object im = immune.get(t);
            if (im != null) et.append(",\"immune\":").append(q(((net.minecraft.tags.TagKey<?>) im).location().getPath()));
            if (DefaultAttributes.hasSupplier(t)) {
                @SuppressWarnings("unchecked")
                AttributeSupplier sup = DefaultAttributes.getSupplier((EntityType<? extends LivingEntity>) t);
                StringBuilder a = new StringBuilder();
                for (var h : BuiltInRegistries.ATTRIBUTE.asHolderIdMap()) {
                    if (!sup.hasAttribute(h)) continue;
                    if (a.length() > 0) a.append(',');
                    a.append(q(h.unwrapKey().get().identifier().getPath())).append(':').append(num(sup.getBaseValue(h)));
                }
                et.append(",\"attr\":{").append(a).append("}");
            }
            et.append("}");
        }
        out.append("\"entity_types\":[").append(et).append("],");

        // ---------- synched entity data accessors (class -> field, id, serializer) ----------
        Map<Integer, String> serNames = new TreeMap<>();
        for (Field f : EntityDataSerializers.class.getFields()) {
            if (!Modifier.isStatic(f.getModifiers())) continue;
            Object v = f.get(null);
            if (v instanceof net.minecraft.network.syncher.EntityDataSerializer<?> s) {
                int sid = EntityDataSerializers.getSerializedId(s);
                if (sid >= 0) serNames.put(sid, f.getName());
            }
        }
        StringBuilder sn = new StringBuilder();
        for (var e : serNames.entrySet()) {
            if (sn.length() > 0) sn.append(',');
            sn.append(q(e.getValue())).append(':').append(e.getKey());
        }
        out.append("\"serializers\":{").append(sn).append("},");
        StringBuilder ed = new StringBuilder();
        Set<String> seen = new TreeSet<>();
        try (JarFile jar = new JarFile(args[0])) {
            List<String> names = new ArrayList<>();
            jar.stream().forEach(e -> {
                String nm = e.getName();
                if (nm.startsWith("net/minecraft/world/entity/") && nm.endsWith(".class")) names.add(nm);
            });
            Collections.sort(names);
            for (String nm : names) {
                String cn = nm.substring(0, nm.length() - 6).replace('/', '.');
                Class<?> c;
                try { c = Class.forName(cn, true, MobDump.class.getClassLoader()); } catch (Throwable t) { continue; }
                StringBuilder fs = new StringBuilder();
                for (Field f : c.getDeclaredFields()) {
                    if (!Modifier.isStatic(f.getModifiers()) || f.getType() != EntityDataAccessor.class) continue;
                    f.setAccessible(true);
                    EntityDataAccessor<?> acc = (EntityDataAccessor<?>) f.get(null);
                    if (fs.length() > 0) fs.append(',');
                    fs.append("[").append(q(f.getName())).append(',').append(acc.id()).append(',')
                      .append(EntityDataSerializers.getSerializedId(acc.serializer())).append("]");
                }
                if (fs.length() == 0) continue;
                String simple = c.getName().substring(c.getName().lastIndexOf('.') + 1).replace('$', '_');
                if (!seen.add(simple)) continue;
                if (ed.length() > 0) ed.append(',');
                ed.append(q(simple)).append(":{\"super\":").append(q(c.getSuperclass().getSimpleName())).append(",\"f\":[").append(fs).append("]}");
            }
        }
        out.append("\"entity_data\":{").append(ed).append("}");
        out.append("}");
        System.out.print(out);
    }
}
