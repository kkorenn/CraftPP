package prism.bridge;

import org.bukkit.Server;
import org.bukkit.World;
import org.bukkit.entity.Player;

import java.lang.classfile.ClassFile;
import java.lang.classfile.CodeBuilder;
import java.lang.classfile.TypeKind;
import java.lang.constant.ClassDesc;
import java.lang.constant.ConstantDescs;
import java.lang.constant.MethodTypeDesc;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Generates the CraftBukkit-shaped classes plugins reach for by name:
 * org.bukkit.craftbukkit.CraftServer, CraftWorld and entity.CraftPlayer. Each
 * implements its Bukkit interface by delegating every method to a ProxyBase
 * (same semantics as the java.lang.reflect.Proxy path), so plugins that cast
 * to CraftPlayer for getHandle() or read the server's package name for version
 * detection see what Paper gives them. Built with java.lang.classfile, no
 * extra dependency.
 */
public final class CraftClasses {
    /** Implemented by every generated class. */
    public interface Generated {
        InvocationHandler prismHandler();

        void prismHandler(InvocationHandler handler);
    }

    static final Map<Class<?>, Class<?>> BY_INTERFACE = new HashMap<>();
    private static final Map<List<Class<?>>, Class<?>> BY_INTERFACES = new HashMap<>();
    private static final Map<Class<?>, MethodHandles.Lookup> LOOKUPS = new HashMap<>();
    /** name+descriptor of every delegate a generated class (including its generated superclasses) implements. */
    private static final Map<Class<?>, Set<String>> IMPLEMENTED = new HashMap<>();
    private static final Map<Method, MethodHandle> DEFAULTS = new ConcurrentHashMap<>();
    private static boolean nmsHandles;

    private static final ClassDesc CD_HANDLER = ClassDesc.of("java.lang.reflect.InvocationHandler");
    private static final ClassDesc CD_METHOD = ClassDesc.of("java.lang.reflect.Method");
    private static final ClassDesc CD_METHODS = CD_METHOD.arrayType();
    private static final MethodTypeDesc MTD_INVOKE = MethodTypeDesc.of(ConstantDescs.CD_Object, ConstantDescs.CD_Object, CD_METHOD,
            ConstantDescs.CD_Object.arrayType());

    private CraftClasses() {
    }

    /** Defines the three classes once; handle getters are typed to NMS only when the server jar is on the classpath. */
    static synchronized void generateAll(boolean nms) {
        if (!BY_INTERFACE.isEmpty()) {
            return;
        }
        nmsHandles = nms;
        define(org.bukkit.craftbukkit.PrismAnchor.class, "org.bukkit.craftbukkit.CraftServer", Server.class, List.of(),
                nms ? "net.minecraft.server.MinecraftServer" : null, "getServer", null);
        define(org.bukkit.craftbukkit.PrismAnchor.class, "org.bukkit.craftbukkit.CraftWorld", World.class, List.of(),
                nms ? "net.minecraft.server.level.ServerLevel" : null, "getHandle", null);
        Class<?> entity = define(org.bukkit.craftbukkit.entity.PrismAnchor.class, "org.bukkit.craftbukkit.entity.CraftEntity",
                org.bukkit.entity.Entity.class, List.of(), nms ? "net.minecraft.world.entity.Entity" : null, "getHandle", null);
        Class<?> living = define(org.bukkit.craftbukkit.entity.PrismAnchor.class, "org.bukkit.craftbukkit.entity.CraftLivingEntity",
                org.bukkit.entity.LivingEntity.class, List.of(), nms ? "net.minecraft.world.entity.LivingEntity" : null, "getHandle", entity);
        define(org.bukkit.craftbukkit.entity.PrismAnchor.class, "org.bukkit.craftbukkit.entity.CraftPlayer", Player.class, List.of(),
                nms ? "net.minecraft.server.level.ServerPlayer" : null, "getHandle", living);
    }

    /** The generated class for an interface (plus extra interfaces); entity interfaces are generated on first use as Craft&lt;Name&gt;. */
    static synchronized Class<?> forInterface(Class<?> iface, Class<?>... extras) {
        if (extras.length == 0) {
            Class<?> known = BY_INTERFACE.get(iface);
            if (known != null || BY_INTERFACE.isEmpty() || !org.bukkit.entity.Entity.class.isAssignableFrom(iface)) {
                return known;
            }
        }
        List<Class<?>> key = new ArrayList<>();
        key.add(iface);
        key.addAll(List.of(extras));
        Class<?> known = BY_INTERFACES.get(key);
        if (known != null || BY_INTERFACE.isEmpty() || !org.bukkit.entity.Entity.class.isAssignableFrom(iface)) {
            return known;
        }
        Class<?> parent = org.bukkit.entity.LivingEntity.class.isAssignableFrom(iface)
                ? BY_INTERFACE.get(org.bukkit.entity.LivingEntity.class) : BY_INTERFACE.get(org.bukkit.entity.Entity.class);
        String name = "org.bukkit.craftbukkit.entity.Craft" + iface.getSimpleName() + (extras.length == 0 ? "" : "$" + extras.length);
        Class<?> generated = define(org.bukkit.craftbukkit.entity.PrismAnchor.class, name, iface, List.of(extras), null, null, parent);
        BY_INTERFACES.put(key, generated);
        return generated;
    }

    /** Runs an interface default method on a generated instance (the equivalent of InvocationHandler.invokeDefault). */
    static Object invokeDefault(Object self, Method method, Object[] args) throws Throwable {
        MethodHandle handle = DEFAULTS.get(method);
        if (handle == null) {
            MethodHandles.Lookup lookup = LOOKUPS.get(self.getClass());
            handle = lookup.findSpecial(method.getDeclaringClass(), method.getName(),
                    MethodType.methodType(method.getReturnType(), method.getParameterTypes()), self.getClass());
            DEFAULTS.put(method, handle);
        }
        return handle.bindTo(self).invokeWithArguments(args == null ? new Object[0] : args);
    }

    private static Class<?> define(Class<?> anchor, String name, Class<?> iface, List<Class<?>> extras, String handleType, String handleMethod,
                                   Class<?> parent) {
        List<Method> methods = new ArrayList<>();
        Set<String> seen = new HashSet<>(parent == null ? Set.of() : IMPLEMENTED.get(parent));
        Set<String> implemented = new HashSet<>(seen);
        if (parent == null) {
            try {
                for (String objectMethod : new String[] {"equals", "hashCode", "toString"}) {
                    Method method = objectMethod.equals("equals") ? Object.class.getMethod(objectMethod, Object.class) : Object.class.getMethod(objectMethod);
                    methods.add(method);
                    seen.add(method.getName() + descriptor(method).descriptorString());
                }
            } catch (NoSuchMethodException e) {
                throw new IllegalStateException(e);
            }
        }
        List<Class<?>> all = new ArrayList<>();
        all.add(iface);
        all.addAll(extras);
        for (Class<?> type : all) {
            for (Method method : type.getMethods()) {
                if (Modifier.isStatic(method.getModifiers())) {
                    continue;
                }
                String key = method.getName() + descriptor(method).descriptorString();
                if (seen.add(key)) {
                    methods.add(method);
                }
                implemented.add(key);
            }
        }

        ClassDesc self = ClassDesc.of(name);
        // The root of a chain owns the handler field; subclasses inherit it (same field, same static METHODS layout per class).
        ClassDesc root = parent == null ? self : ClassDesc.of(rootOf(parent).getName());
        ClassDesc superDesc = parent == null ? ConstantDescs.CD_Object : ClassDesc.of(parent.getName());
        byte[] bytes = ClassFile.of().build(self, cb -> {
            cb.withFlags(ClassFile.ACC_PUBLIC | ClassFile.ACC_SUPER);
            cb.withSuperclass(superDesc);
            // Every superinterface is declared directly so default methods can be invoked with invokespecial.
            java.util.LinkedHashSet<Class<?>> closure = new java.util.LinkedHashSet<>();
            for (Class<?> type : all) {
                allInterfaces(type, closure);
            }
            List<ClassDesc> interfaces = new ArrayList<>();
            for (Class<?> type : closure) {
                interfaces.add(desc(type));
            }
            if (parent == null) {
                interfaces.add(ClassDesc.of(Generated.class.getName()));
            }
            cb.withInterfaceSymbols(interfaces);
            cb.withField("METHODS", CD_METHODS, ClassFile.ACC_PUBLIC | ClassFile.ACC_STATIC);
            cb.withMethodBody(ConstantDescs.INIT_NAME, ConstantDescs.MTD_void, ClassFile.ACC_PUBLIC, code -> {
                code.aload(0);
                code.invokespecial(superDesc, ConstantDescs.INIT_NAME, ConstantDescs.MTD_void);
                code.return_();
            });
            if (parent == null) {
                cb.withField("prismHandler", CD_HANDLER, ClassFile.ACC_PUBLIC);
                cb.withMethodBody("prismHandler", MethodTypeDesc.of(CD_HANDLER), ClassFile.ACC_PUBLIC, code -> {
                    code.aload(0);
                    code.getfield(self, "prismHandler", CD_HANDLER);
                    code.areturn();
                });
                cb.withMethodBody("prismHandler", MethodTypeDesc.of(ConstantDescs.CD_void, CD_HANDLER), ClassFile.ACC_PUBLIC, code -> {
                    code.aload(0);
                    code.aload(1);
                    code.putfield(self, "prismHandler", CD_HANDLER);
                    code.return_();
                });
            }
            if (handleType != null) {
                ClassDesc handle = ClassDesc.of(handleType);
                cb.withMethodBody(handleMethod, MethodTypeDesc.of(handle), ClassFile.ACC_PUBLIC, code -> {
                    code.aload(0);
                    code.invokestatic(ClassDesc.of("prism.bridge.ProxyBase"), "handleOf",
                            MethodTypeDesc.of(ConstantDescs.CD_Object, ConstantDescs.CD_Object));
                    code.checkcast(handle);
                    code.areturn();
                });
            }
            for (int i = 0; i < methods.size(); i++) {
                Method method = methods.get(i);
                int index = i;
                cb.withMethodBody(method.getName(), descriptor(method), ClassFile.ACC_PUBLIC, code -> emitDelegate(code, self, root, method, index));
            }
        });

        try {
            MethodHandles.Lookup lookup = MethodHandles.privateLookupIn(anchor, MethodHandles.lookup());
            Class<?> defined = lookup.defineClass(bytes);
            defined.getField("METHODS").set(null, methods.toArray(new Method[0]));
            if (extras.isEmpty()) {
                BY_INTERFACE.put(iface, defined);
            }
            IMPLEMENTED.put(defined, implemented);
            LOOKUPS.put(defined, MethodHandles.privateLookupIn(defined, lookup));
            return defined;
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("Could not define " + name, e);
        }
    }

    private static Class<?> rootOf(Class<?> generated) {
        Class<?> root = generated;
        while (root.getSuperclass() != Object.class) {
            root = root.getSuperclass();
        }
        return root;
    }

    private static Set<Class<?>> allInterfaces(Class<?> type, Set<Class<?>> into) {
        if (into.add(type)) {
            for (Class<?> parent : type.getInterfaces()) {
                allInterfaces(parent, into);
            }
        }
        return into;
    }

    private static void emitDelegate(CodeBuilder code, ClassDesc self, ClassDesc root, Method method, int index) {
        code.aload(0);
        code.getfield(root, "prismHandler", CD_HANDLER);
        code.aload(0);
        code.getstatic(self, "METHODS", CD_METHODS);
        code.loadConstant(index);
        code.aaload();
        Class<?>[] parameters = method.getParameterTypes();
        code.loadConstant(parameters.length);
        code.anewarray(ConstantDescs.CD_Object);
        int slot = 1;
        for (int p = 0; p < parameters.length; p++) {
            code.dup();
            code.loadConstant(p);
            TypeKind kind = TypeKind.from(desc(parameters[p]));
            code.loadLocal(kind, slot);
            box(code, parameters[p]);
            code.aastore();
            slot += kind.slotSize();
        }
        code.invokeinterface(CD_HANDLER, "invoke", MTD_INVOKE);
        Class<?> returned = method.getReturnType();
        if (returned == void.class) {
            code.pop();
            code.return_();
        } else if (returned.isPrimitive()) {
            unbox(code, returned);
            code.return_(TypeKind.from(desc(returned)));
        } else {
            code.checkcast(desc(returned));
            code.areturn();
        }
    }

    private static void box(CodeBuilder code, Class<?> type) {
        if (!type.isPrimitive()) {
            return;
        }
        Class<?> wrapper = wrapper(type);
        code.invokestatic(desc(wrapper), "valueOf", MethodTypeDesc.of(desc(wrapper), desc(type)));
    }

    private static void unbox(CodeBuilder code, Class<?> type) {
        Class<?> wrapper = wrapper(type);
        code.checkcast(desc(wrapper));
        code.invokevirtual(desc(wrapper), type.getName() + "Value", MethodTypeDesc.of(desc(type)));
    }

    private static Class<?> wrapper(Class<?> primitive) {
        if (primitive == boolean.class) return Boolean.class;
        if (primitive == byte.class) return Byte.class;
        if (primitive == char.class) return Character.class;
        if (primitive == short.class) return Short.class;
        if (primitive == int.class) return Integer.class;
        if (primitive == long.class) return Long.class;
        if (primitive == float.class) return Float.class;
        if (primitive == double.class) return Double.class;
        throw new IllegalArgumentException(primitive.getName());
    }

    private static ClassDesc desc(Class<?> type) {
        return ClassDesc.ofDescriptor(type.descriptorString());
    }

    private static MethodTypeDesc descriptor(Method method) {
        ClassDesc[] parameters = new ClassDesc[method.getParameterCount()];
        Class<?>[] types = method.getParameterTypes();
        for (int i = 0; i < types.length; i++) {
            parameters[i] = desc(types[i]);
        }
        return MethodTypeDesc.of(desc(method.getReturnType()), parameters);
    }
}
