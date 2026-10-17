#pragma once
// Enchantment effect components (net.minecraft.world.item.enchantment, 26.x data-driven effects).
inline float enchant_damage_protection(LivingEntity&, const DamageSource&) { return 0.0f; }  // filled in by the enchantment port
inline int enchant_block_experience(const ItemStack&, int amount) { return amount; }  // processBlockExperience (silk touch)
