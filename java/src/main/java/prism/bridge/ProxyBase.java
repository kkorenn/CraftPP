package prism.bridge;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Implements a Bukkit interface by forwarding each call to a same-named method
 * on a plain backing object. Interface default methods run as written; anything
 * the backing object lacks throws UnsupportedOperationException naming the
 * missing method, so a plugin hitting an unimplemented corner fails loudly
 * instead of silently misbehaving.
 */
public final class ProxyBase implements InvocationHandler {
    private static final Object MISSING = new Object();
    private final Object target;
    private final Map<Method, Object> resolved = new ConcurrentHashMap<>();

    private ProxyBase(Object target) {
        this.target = target;
    }

    @SuppressWarnings("unchecked")
    public static <T> T create(Class<T> iface, Object target, Class<?>... extraInterfaces) {
        Class<?>[] interfaces = new Class<?>[extraInterfaces.length + 1];
        interfaces[0] = iface;
        System.arraycopy(extraInterfaces, 0, interfaces, 1, extraInterfaces.length);
        T proxy;
        Class<?> generated = CraftClasses.forInterface(iface, extraInterfaces);
        if (generated != null) {
            try {
                CraftClasses.Generated instance = (CraftClasses.Generated) generated.getConstructor().newInstance();
                instance.prismHandler(new ProxyBase(target));
                proxy = (T) instance;
            } catch (ReflectiveOperationException e) {
                throw new IllegalStateException(e);
            }
        } else {
            proxy = (T) Proxy.newProxyInstance(ProxyBase.class.getClassLoader(), interfaces, new ProxyBase(target));
        }
        if (target instanceof Backing backing) {
            backing.attachProxy(proxy);
        }
        return proxy;
    }

    /** The backing object behind a proxy, or the object itself if it is not one of ours. */
    public static Object unwrap(Object maybeProxy) {
        if (maybeProxy instanceof CraftClasses.Generated generated && generated.prismHandler() instanceof ProxyBase base) {
            return base.target;
        }
        if (maybeProxy != null && Proxy.isProxyClass(maybeProxy.getClass())) {
            InvocationHandler handler = Proxy.getInvocationHandler(maybeProxy);
            if (handler instanceof ProxyBase base) {
                return base.target;
            }
        }
        return maybeProxy;
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        Object entry = resolved.computeIfAbsent(method, m -> {
            try {
                Method found = target.getClass().getMethod(m.getName(), m.getParameterTypes());
                found.setAccessible(true);
                return found;
            } catch (NoSuchMethodException e) {
                return MISSING;
            }
        });
        if (entry instanceof Method found) {
            try {
                return found.invoke(target, args);
            } catch (InvocationTargetException e) {
                throw e.getCause();
            }
        }
        if (method.isDefault()) {
            return proxy instanceof CraftClasses.Generated ? CraftClasses.invokeDefault(proxy, method, args)
                    : InvocationHandler.invokeDefault(proxy, method, args);
        }
        switch (method.getName()) {
            case "equals" -> {
                return proxy == args[0] || target.equals(unwrap(args[0]));
            }
            case "hashCode" -> {
                return target.hashCode();
            }
            case "toString" -> {
                return target.toString();
            }
            default -> {
            }
        }
        // Common "give me nothing" shapes so read-only queries on unsupported
        // surfaces degrade gracefully instead of crashing whole plugins.
        Class<?> returnType = method.getReturnType();
        if (returnType == Optional.class) {
            return Optional.empty();
        }
        throw new UnsupportedOperationException(
                "CraftPP does not implement " + method.getDeclaringClass().getSimpleName() + "." + method.getName() + " yet");
    }

    /** Called by generated CraftPlayer.getHandle()/CraftServer.getServer(): the NMS object behind a proxy, or null. */
    public static Object handleOf(Object proxy) {
        Object target = unwrap(proxy);
        Object handle = target instanceof HandleProvider provider ? provider.nmsHandle() : null;
        if (handle == null) {
            throw new UnsupportedOperationException("CraftPP has no server internals (NMS) for " + target.getClass().getSimpleName()
                    + (prism.bridge.nms.Nms.enabled() ? "" : "; no plugin in plugins/ referenced net.minecraft.* at startup"));
        }
        return handle;
    }

    /** Backing objects with an NMS counterpart. */
    public interface HandleProvider {
        Object nmsHandle();
    }

    /** Backing objects that need a handle on their own proxy (to return "this" as the interface). */
    public interface Backing {
        void attachProxy(Object proxy);
    }
}
