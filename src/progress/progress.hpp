#pragma once
// Internal header of the progression module (advancements.cpp, recipe_book.cpp, stats.cpp). Public entry points:
// api.hpp. Data (advancements, recipes) is read from data/vanilla at startup on a background thread and kept
// compiled; parse-time JSON is freed. Everything else runs on the tick thread.
#include "api.hpp"
#include "../mobs/mobs.hpp"

#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace progress {

inline const std::string VANILLA = "data/vanilla/minecraft/";
inline std::string ns(const std::string& id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; }

// ---------- registries + tags (data/vanilla/minecraft/tags/<registry>/) ----------
using Ids = std::shared_ptr<const std::vector<uint16_t>>;  // sorted registry ids
int reg_id(const std::string& reg, const std::string& id);  // "item", "block", "entity_type", "worldgen/biome", ...
// HolderSet as JSON writes it: "#tag", "id" or ["id", ...]. Resolved ids (unknown entries dropped); `tag` receives
// the tag id when it was a tag reference (the network encodes named sets by name).
Ids holder_set(const std::string& reg, const Json& j, std::string* tag = nullptr);
std::vector<std::string> holder_names(const std::string& reg, const Json& j);  // registries without numeric ids
inline bool has(const Ids& s, int id) { return s && std::binary_search(s->begin(), s->end(), uint16_t(id)); }

// ---------- predicates (net.minecraft.advancements.predicates) ----------
struct IRange {  // MinMaxBounds.Ints
    int64_t lo = INT64_MIN, hi = INT64_MAX;
    bool any() const { return lo == INT64_MIN && hi == INT64_MAX; }
    bool test(int64_t v) const { return v >= lo && v <= hi; }
};
struct DRange {  // MinMaxBounds.Doubles
    std::optional<double> lo, hi;
    bool test(double v) const { return !(lo && *lo > v) && !(hi && *hi < v); }
    bool test_sqr(double v) const { return !(lo && *lo * *lo > v) && !(hi && *hi * *hi < v); }
};
IRange irange(const Json* j);
DRange drange(const Json* j);

struct ItemPred {
    Ids items;  // null: any item
    IRange count;
    bool bad = false;  // uses a part CraftPP can't evaluate (components, enchantments): never matches
    bool test(const ItemStack& s) const;
};
ItemPred item_pred(const Json& j);

struct BlockPred {
    Ids blocks;
    std::vector<StateMatch> state;
    bool bad = false;
    bool test(int state) const;
};
struct LocPred {  // LocationPredicate
    DRange x, y, z;
    Ids biomes;
    std::vector<std::string> structures;
    int dim = -1, smokey = -1;
    std::optional<BlockPred> block;
    std::optional<BlockPred> fluid;  // FluidPredicate: fluid "blocks" are water / lava states
    bool bad = false;
    bool test(World* w, double px, double py, double pz) const;
};
struct DistPred {  // DistancePredicate
    DRange x, y, z, horizontal, absolute;
    bool test(double x0, double y0, double z0, double x1, double y1, double z1) const;
};
struct EffectPred { int effect = 0; IRange amp, dur; int ambient = -1, visible = -1; };
struct EntPred {  // EntityPredicate: map of sub-predicates
    Ids types;
    std::unique_ptr<LocPred> loc, stepping;
    std::unique_ptr<DistPred> dist;
    std::vector<std::pair<std::string, bool>> flags;
    std::vector<std::pair<int, ItemPred>> equipment;  // EquipSlot -> predicate
    std::vector<EffectPred> effects;
    bool bad = false;
    bool test(Entity* e, World* w, const Vec3& origin) const;
};
std::unique_ptr<EntPred> ent_pred(const Json& j);

struct DamagePred {  // DamageSourcePredicate
    std::vector<std::pair<std::vector<std::string>, bool>> tags;  // damage type names of the tag, expected
    std::unique_ptr<EntPred> direct, source;
    int is_direct = -1;
    bool test(const DamageSource& s, World* w, const Vec3& origin) const;
};

// Loot conditions of a ContextAwarePredicate ("player", "entity", "location", ...): all must hold.
struct LCtx {
    Entity* self = nullptr;
    World* level = nullptr;
    Vec3 origin{0, 0, 0};
    int state = -1;                 // BLOCK_STATE (location predicates)
    const ItemStack* tool = nullptr;  // TOOL
};
struct LCond {
    enum T : uint8_t { FALSE_, ENTITY, INVERTED, ANY_OF, ALL_OF, LOCATION, BLOCK_STATE, MATCH_TOOL, RANDOM } t = FALSE_;
    std::shared_ptr<EntPred> ent;
    std::vector<LCond> kids;
    std::shared_ptr<LocPred> loc;
    int off[3] = {0, 0, 0};
    BlockPred block;
    ItemPred tool;
    float chance = 0;
    bool test(const LCtx& c) const;
};
using Conds = std::vector<LCond>;
Conds conds(const Json* j);  // object or list of conditions; null = none
inline bool test_all(const Conds& cs, const LCtx& c) {
    for (auto& x : cs) if (!x.test(c)) return false;
    return true;
}
LCtx entity_ctx(Player& p, Entity* e);  // THIS = e, ORIGIN = the player's position

// ---------- advancements ----------
enum Trigger : uint8_t {
    T_IMPOSSIBLE, T_TICK, T_LOCATION, T_SLEPT_IN_BED, T_HERO_OF_THE_VILLAGE, T_INVENTORY_CHANGED, T_RECIPE_UNLOCKED,
    T_RECIPE_CRAFTED, T_PLAYER_KILLED_ENTITY, T_ENTITY_KILLED_PLAYER, T_CHANGED_DIMENSION, T_ENTER_BLOCK,
    T_PLACED_BLOCK, T_ITEM_USED_ON_BLOCK, T_CONSUME_ITEM, T_BRED_ANIMALS, T_TAME_ANIMAL, T_VILLAGER_TRADE,
    T_BREWED_POTION, T_ENCHANTED_ITEM, T_EFFECTS_CHANGED, T_SUMMONED_ENTITY, T_USED_ENDER_EYE, T_CONSTRUCT_BEACON,
    T_LEVITATION, T_NETHER_TRAVEL, T_UNSUPPORTED, T_COUNT
};

struct Criterion {
    uint16_t adv = 0;
    uint8_t trigger = T_UNSUPPORTED;
    bool bad = false;  // a condition CraftPP can't evaluate: never matches (the advancement stays obtainable by command)
    std::string name;
    Conds player, entity, parent, partner, child, source, location;
    std::optional<DamagePred> killing_blow;
    std::vector<ItemPred> items;
    std::optional<ItemPred> item;
    IRange occupied, full, empty, levels, level, duration;
    std::optional<DRange> range;     // used_ender_eye distance
    std::optional<DistPred> distance;
    std::optional<LocPred> start;
    std::vector<int> recipes;        // recipe registry indices
    int from = -1, to = -1;
    Ids block, potions;
    std::vector<StateMatch> state;
    std::vector<EffectPred> effects;
};

struct Display {
    Nbt title, description;
    int icon = -1, icon_count = 1, frame = 0;  // AdvancementType: 0 task, 1 challenge, 2 goal
    bool toast = true, announce = true, hidden = false;
    std::string background;
};
struct Advancement {
    std::string id;
    int parent = -1;
    std::vector<int> children;
    std::unique_ptr<Display> display;
    int first = 0, count = 0;              // criteria[first, first + count)
    std::vector<std::vector<int>> req;     // requirements: global criterion indices
    int xp = 0;
    std::vector<std::string> loot;
    std::vector<int> reward_recipes;
    std::string function;
    float x = 0, y = 0;                    // TreeNodePosition
    bool telemetry = false;
    int root() const;
};
inline std::vector<Advancement> advs;
inline std::vector<Criterion> criteria;
inline std::unordered_map<std::string, int> adv_by_id;
inline std::array<std::vector<int>, T_COUNT> by_trigger;  // criterion indices per trigger
int find_adv(const std::string& id);
Nbt adv_name(int a);  // Advancement.name: [title] with the description on hover, or the id

// ---------- recipes (RecipeManager) ----------
struct Ingredient {
    std::string tag;  // named HolderSet (network: by name); empty = direct list
    Ids items;
    bool test(int item) const { return has(items, item); }
};
struct RecipeDef {
    std::string id;
    bool special = false, notify = true, shaped = false;
    int category = 0, group = -1, w = 3, h = 3;
    std::vector<Ingredient> place;   // PlacementInfo.ingredients
    std::vector<int> slots;          // PlacementInfo.slotsToIngredientIndex
    std::vector<int> displays;       // indices into recipe_displays
    int craft = -1;                  // ::recipes index (crafting table / 2x2 matching)
};
struct DisplayDef {
    int recipe = 0;
    std::vector<uint8_t> display;  // RecipeDisplay network encoding
    std::vector<uint8_t> entry;    // RecipeDisplayEntry: id, display, group, category, requirements
};
inline std::vector<RecipeDef> recipe_defs;  // sorted by id
inline std::vector<DisplayDef> recipe_displays;
inline std::unordered_map<std::string, int> recipe_by_id;
inline std::vector<int> recipe_of_craft;  // ::recipes index -> recipe_defs index
int find_recipe(const std::string& id);
void load_recipes();

// ---------- per-player state ----------
struct State {
    // advancements (PlayerAdvancements)
    std::vector<int64_t> obtained;  // per criterion: ms since the epoch, 0 = not done
    std::vector<uint8_t> started;   // advancement has an AdvancementProgress (sent / saved)
    std::vector<uint8_t> visible;
    std::set<int> changed, roots;
    bool first_packet = true;
    int tab = -1;
    // recipe book (ServerRecipeBook)
    std::vector<uint8_t> known, highlight;
    bool book[4][2] = {};  // RecipeBookType -> open, filtering
    // stats (ServerStatsCounter): (type << 24 | id) -> value
    std::unordered_map<uint32_t, int> stats;
    std::set<uint32_t> dirty;
    // InventoryMenu lastSlots for inventory_changed; levitation / nether travel bookkeeping
    std::array<ItemStack, 41> last_inv{};
    std::optional<Vec3> entered_nether;
    bool loaded = false;
};
State& st(Player& p);  // created on first use
void wait_loaded();    // blocks until the background data load finished

// advancements.cpp
bool is_done(const State& s, int a);
bool award(Player& p, int a, int crit);
bool revoke(Player& p, int a, int crit);
void flush(Player& p, bool show);
void trigger(Player& p, int t, const std::function<bool(const Criterion&)>& match);
void load_advancements_file(Player& p);
void save_advancements_file(const Player& p);
void advancements_init();
void tick_player(Player& p);
// recipe_book.cpp
int add_recipes(Player& p, const std::vector<int>& rs);
int remove_recipes(Player& p, const std::vector<int>& rs);
void send_recipe_book(Player& p);
void recipe_book_init();
// stats.cpp
void stats_init();
void load_stats_file(Player& p);
void save_stats_file(const Player& p);
void send_stats(Player& p);

// small helpers
std::string uuid_str(const Player& p);  // xxxxxxxx-xxxx-... (file names)
std::string json_quote(const std::string& s);
int64_t now_ms();
}  // namespace progress
