package prism.bridge;

import org.bukkit.NamespacedKey;
import org.bukkit.persistence.PersistentDataAdapterContext;
import org.bukkit.persistence.PersistentDataContainer;
import org.bukkit.persistence.PersistentDataType;

import java.util.Collections;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Persistent data container. Values are stored in their primitive form
 * exactly like CraftBukkit's; onChange pushes them to PrismMC, which keeps
 * player/world/chunk/entity containers in its own save data (Persistence).
 */
public final class PdcImpl implements PersistentDataContainer, PersistentDataAdapterContext {
    private final Map<NamespacedKey, Object> values = new ConcurrentHashMap<>();
    private Runnable onChange;

    /** Called after every mutation so owners can persist the container. */
    void onChange(Runnable listener) {
        onChange = listener;
    }

    private void changed() {
        if (onChange != null) {
            onChange.run();
        }
    }

    Iterable<Map.Entry<NamespacedKey, Object>> rawEntries() {
        return values.entrySet();
    }

    void putRaw(NamespacedKey key, Object primitive) {
        values.put(key, primitive);
    }

    @Override
    public <P, C> void set(NamespacedKey key, PersistentDataType<P, C> type, C value) {
        values.put(key, type.toPrimitive(value, this));
        changed();
    }

    @Override
    public <P, C> boolean has(NamespacedKey key, PersistentDataType<P, C> type) {
        Object raw = values.get(key);
        return raw != null && type.getPrimitiveType().isInstance(raw);
    }

    @Override
    public boolean has(NamespacedKey key) {
        return values.containsKey(key);
    }

    @Override
    public <P, C> C get(NamespacedKey key, PersistentDataType<P, C> type) {
        Object raw = values.get(key);
        if (raw == null || !type.getPrimitiveType().isInstance(raw)) {
            return null;
        }
        return type.fromPrimitive(type.getPrimitiveType().cast(raw), this);
    }

    @Override
    public <P, C> C getOrDefault(NamespacedKey key, PersistentDataType<P, C> type, C defaultValue) {
        C value = get(key, type);
        return value == null ? defaultValue : value;
    }

    @Override
    public Set<NamespacedKey> getKeys() {
        return Collections.unmodifiableSet(values.keySet());
    }

    @Override
    public void remove(NamespacedKey key) {
        values.remove(key);
        changed();
    }

    @Override
    public int getSize() {
        return values.size();
    }

    @Override
    public boolean isEmpty() {
        return values.isEmpty();
    }

    @Override
    public void copyTo(PersistentDataContainer other, boolean replace) {
        if (other instanceof PdcImpl target) {
            if (replace) {
                target.values.putAll(values);
            } else {
                values.forEach(target.values::putIfAbsent);
            }
            target.changed();
        }
    }

    @Override
    public PersistentDataAdapterContext getAdapterContext() {
        return this;
    }

    @Override
    public PersistentDataContainer newPersistentDataContainer() {
        return new PdcImpl();
    }

    @Override
    public byte[] serializeToBytes() {
        throw new UnsupportedOperationException("CraftPP persistent data containers cannot be serialized yet");
    }

    @Override
    public void readFromBytes(byte[] bytes, boolean clear) {
        throw new UnsupportedOperationException("CraftPP persistent data containers cannot be deserialized yet");
    }
}
