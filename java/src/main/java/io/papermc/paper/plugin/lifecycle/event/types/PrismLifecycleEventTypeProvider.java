package io.papermc.paper.plugin.lifecycle.event.types;

import io.papermc.paper.plugin.lifecycle.event.LifecycleEvent;
import io.papermc.paper.plugin.lifecycle.event.LifecycleEventOwner;
import io.papermc.paper.plugin.lifecycle.event.handler.LifecycleEventHandler;
import io.papermc.paper.plugin.lifecycle.event.handler.configuration.MonitorLifecycleEventHandlerConfiguration;
import io.papermc.paper.plugin.lifecycle.event.handler.configuration.PrioritizedLifecycleEventHandlerConfiguration;

/**
 * PrismMC's {@code LifecycleEventTypeProvider}. The interface is package-private
 * in paper-api, so this lives in its package. It lets {@code LifecycleEvents}
 * initialise and plugins register handlers; handlers are recorded but never
 * invoked because Brigadier command registration is not modelled yet.
 */
public final class PrismLifecycleEventTypeProvider implements LifecycleEventTypeProvider {
    @Override
    public <O extends LifecycleEventOwner, E extends LifecycleEvent> LifecycleEventType.Monitorable<O, E> monitor(String name, Class<? extends O> ownerType) {
        return new Monitorable<>(name);
    }

    @Override
    public <O extends LifecycleEventOwner, E extends LifecycleEvent> LifecycleEventType.Prioritizable<O, E> prioritized(String name, Class<? extends O> ownerType) {
        return new Prioritizable<>(name);
    }

    @Override
    public TagEventTypeProvider tagProvider() {
        // Tag registrars are accepted (LifecycleEvents initialises them eagerly) but never fired.
        return new TagEventTypeProvider() {
            @Override
            public <T> LifecycleEventType.Prioritizable<io.papermc.paper.plugin.bootstrap.BootstrapContext, io.papermc.paper.plugin.lifecycle.event.registrar.ReloadableRegistrarEvent<io.papermc.paper.tag.PreFlattenTagRegistrar<T>>> preFlatten(io.papermc.paper.registry.RegistryKey<T> registryKey) {
                return new Prioritizable<>("tags/pre-flatten/" + registryKey.key().asString());
            }

            @Override
            public <T> LifecycleEventType.Prioritizable<io.papermc.paper.plugin.bootstrap.BootstrapContext, io.papermc.paper.plugin.lifecycle.event.registrar.ReloadableRegistrarEvent<io.papermc.paper.tag.PostFlattenTagRegistrar<T>>> postFlatten(io.papermc.paper.registry.RegistryKey<T> registryKey) {
                return new Prioritizable<>("tags/post-flatten/" + registryKey.key().asString());
            }
        };
    }

    /** A registered handler configuration: the bridge reads these back when a plugin registers it. */
    public interface Captured {
        String typeName();

        LifecycleEventHandler<?> handler();

        int priority();
    }

    record Monitorable<O extends LifecycleEventOwner, E extends LifecycleEvent>(String name) implements LifecycleEventType.Monitorable<O, E> {
        @Override
        public MonitorLifecycleEventHandlerConfiguration<O> newHandler(LifecycleEventHandler<? super E> handler) {
            return new MonitorConfig<>(name, handler);
        }
    }

    record Prioritizable<O extends LifecycleEventOwner, E extends LifecycleEvent>(String name) implements LifecycleEventType.Prioritizable<O, E> {
        @Override
        public PrioritizedLifecycleEventHandlerConfiguration<O> newHandler(LifecycleEventHandler<? super E> handler) {
            return new PrioritizedConfig<>(name, handler);
        }
    }

    static final class MonitorConfig<O extends LifecycleEventOwner> implements MonitorLifecycleEventHandlerConfiguration<O>, Captured {
        private final String typeName;
        private final LifecycleEventHandler<?> handler;

        MonitorConfig(String typeName, LifecycleEventHandler<?> handler) {
            this.typeName = typeName;
            this.handler = handler;
        }

        @Override
        public MonitorLifecycleEventHandlerConfiguration<O> monitor() {
            return this;
        }

        @Override
        public String typeName() {
            return typeName;
        }

        @Override
        public LifecycleEventHandler<?> handler() {
            return handler;
        }

        @Override
        public int priority() {
            return Integer.MAX_VALUE;
        }
    }

    static final class PrioritizedConfig<O extends LifecycleEventOwner> implements PrioritizedLifecycleEventHandlerConfiguration<O>, Captured {
        private final String typeName;
        private final LifecycleEventHandler<?> handler;
        private int priority;

        PrioritizedConfig(String typeName, LifecycleEventHandler<?> handler) {
            this.typeName = typeName;
            this.handler = handler;
        }

        @Override
        public PrioritizedLifecycleEventHandlerConfiguration<O> priority(int value) {
            priority = value;
            return this;
        }

        @Override
        public PrioritizedLifecycleEventHandlerConfiguration<O> monitor() {
            priority = Integer.MAX_VALUE;
            return this;
        }

        @Override
        public String typeName() {
            return typeName;
        }

        @Override
        public LifecycleEventHandler<?> handler() {
            return handler;
        }

        @Override
        public int priority() {
            return priority;
        }
    }
}
