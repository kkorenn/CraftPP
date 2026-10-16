package prism.bridge.nms;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.classfile.ClassFile;
import java.lang.classfile.ClassModel;
import java.lang.classfile.ClassTransform;
import java.lang.classfile.FieldTransform;
import java.lang.classfile.MethodTransform;
import java.lang.classfile.attribute.InnerClassInfo;
import java.lang.classfile.attribute.InnerClassesAttribute;
import java.lang.classfile.attribute.RecordAttribute;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Paper ships Mojang's server classes with access transformers applied
 * (fields such as ServerCommonPacketListenerImpl.connection are public
 * there), and NMS plugins are compiled against that. This tool rewrites the
 * official jar once so every class, field and method is public and instance
 * fields are not final: a superset of Paper's transformers. Run by PrismMC
 * the first time NMS plugins are found: java -cp prism-bridge.jar
 * prism.bridge.nms.Widen in.jar out.jar
 */
public final class Widen {
    private static final int VISIBILITY = ClassFile.ACC_PUBLIC | ClassFile.ACC_PROTECTED | ClassFile.ACC_PRIVATE;

    /**
     * Members Paper adds to NMS classes by source patch (not just access) that
     * plugins compile against. Each entry is our own delegating code, keyed by
     * the internal class name. Extend as plugins report NoSuchMethodError.
     */
    private static final java.util.Map<String, java.util.function.Consumer<java.lang.classfile.ClassBuilder>> PAPER_PATCHES = java.util.Map.of(
            "net/minecraft/network/protocol/game/ClientboundPlayerInfoUpdatePacket", cb -> {
                // Paper: public ClientboundPlayerInfoUpdatePacket(EnumSet<Action> actions, Entry entry)
                java.lang.constant.ClassDesc self = java.lang.constant.ClassDesc.of("net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket");
                java.lang.constant.ClassDesc entry = java.lang.constant.ClassDesc.of("net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket$Entry");
                java.lang.constant.ClassDesc enumSet = java.lang.constant.ClassDesc.of("java.util.EnumSet");
                java.lang.constant.ClassDesc list = java.lang.constant.ClassDesc.of("java.util.List");
                cb.withMethodBody(java.lang.constant.ConstantDescs.INIT_NAME,
                        java.lang.constant.MethodTypeDesc.of(java.lang.constant.ConstantDescs.CD_void, enumSet, entry), ClassFile.ACC_PUBLIC, code -> {
                            code.aload(0);
                            code.invokespecial(java.lang.constant.ConstantDescs.CD_Object, java.lang.constant.ConstantDescs.INIT_NAME, java.lang.constant.ConstantDescs.MTD_void);
                            code.aload(0);
                            code.aload(1);
                            code.putfield(self, "actions", enumSet);
                            code.aload(0);
                            code.aload(2);
                            code.invokestatic(list, "of", java.lang.constant.MethodTypeDesc.of(list, java.lang.constant.ConstantDescs.CD_Object), true);
                            code.putfield(self, "entries", list);
                            code.return_();
                        });
            });

    private Widen() {
    }

    public static void main(String[] args) throws IOException {
        Path in = Path.of(args[0]);
        Path out = Path.of(args[1]);
        Path temporary = out.resolveSibling(out.getFileName() + ".tmp");
        Files.createDirectories(out.getParent());
        ClassFile classFile = ClassFile.of();
        int classes = 0;
        try (ZipInputStream input = new ZipInputStream(Files.newInputStream(in));
             ZipOutputStream output = new ZipOutputStream(Files.newOutputStream(temporary))) {
            for (ZipEntry entry; (entry = input.getNextEntry()) != null; ) {
                byte[] bytes = input.readAllBytes();
                String name = entry.getName();
                if (name.startsWith("META-INF/") && (name.endsWith(".SF") || name.endsWith(".RSA") || name.endsWith(".DSA") || name.endsWith(".EC"))) {
                    continue; // Rewritten classes cannot keep Mojang's signature.
                }
                if (name.equals("META-INF/MANIFEST.MF")) {
                    java.util.jar.Manifest manifest = new java.util.jar.Manifest(new java.io.ByteArrayInputStream(bytes));
                    manifest.getEntries().clear(); // Drop per-entry digests, keep the main attributes.
                    java.io.ByteArrayOutputStream rewritten = new java.io.ByteArrayOutputStream();
                    manifest.write(rewritten);
                    bytes = rewritten.toByteArray();
                }
                if (name.endsWith(".class") && !name.equals("module-info.class")) {
                    bytes = widen(classFile, bytes);
                    classes++;
                }
                output.putNextEntry(new ZipEntry(entry.getName()));
                output.write(bytes);
                output.closeEntry();
            }
        }
        Files.move(temporary, out, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        System.out.println("Widened " + classes + " classes into " + out.getFileName());
    }

    static byte[] widen(ClassFile classFile, byte[] bytes) {
        ClassModel model = classFile.parse(bytes);
        boolean record = model.findAttribute(java.lang.classfile.Attributes.record()).isPresent();
        java.util.function.Consumer<java.lang.classfile.ClassBuilder> patch = PAPER_PATCHES.get(model.thisClass().asInternalName());
        return classFile.transformClass(model, ClassTransform.ofStateful(() -> new ClassTransform() {
            @Override
            public void atEnd(java.lang.classfile.ClassBuilder builder) {
                if (patch != null) {
                    patch.accept(builder);
                }
            }

            @Override
            public void accept(java.lang.classfile.ClassBuilder builder, java.lang.classfile.ClassElement element) {
                switch (element) {
                    case java.lang.classfile.AccessFlags flags -> builder.withFlags(publicFlags(flags.flagsMask()));
                    case InnerClassesAttribute inner -> {
                        List<InnerClassInfo> infos = new ArrayList<>();
                        for (InnerClassInfo info : inner.classes()) {
                            infos.add(InnerClassInfo.of(info.innerClass(), info.outerClass(), info.innerName(), publicFlags(info.flagsMask())));
                        }
                        builder.with(InnerClassesAttribute.of(infos));
                    }
                    case java.lang.classfile.FieldModel field -> builder.transformField(field, FieldTransform.ofStateful(() -> (fb, fe) -> {
                        if (fe instanceof java.lang.classfile.AccessFlags flags) {
                            int mask = publicFlags(flags.flagsMask());
                            if (!record && (mask & ClassFile.ACC_STATIC) == 0) {
                                mask &= ~ClassFile.ACC_FINAL;
                            }
                            fb.withFlags(mask);
                        } else {
                            fb.with(fe);
                        }
                    }));
                    case java.lang.classfile.MethodModel method -> builder.transformMethod(method, MethodTransform.ofStateful(() -> (mb, me) -> {
                        if (me instanceof java.lang.classfile.AccessFlags flags) {
                            mb.withFlags(publicFlags(flags.flagsMask()));
                        } else {
                            mb.with(me);
                        }
                    }));
                    default -> builder.with(element);
                }
            }
        }));
    }

    private static int publicFlags(int mask) {
        return (mask & ~VISIBILITY) | ClassFile.ACC_PUBLIC;
    }
}
