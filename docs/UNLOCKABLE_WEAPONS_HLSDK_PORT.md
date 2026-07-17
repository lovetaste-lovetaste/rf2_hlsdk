# Porting the TF2 Weapon Index System to hlsdk-portable (Xash3D)

Companion to `TF2_WEAPON_INDEX_SYSTEM.md` (in the old rooster-fortress repo). That doc explains
how live TF2's defindex/schema/attribute system works; this one gives the concrete steps to build
it in **this** codebase (hlsdk-portable, targeting Xash3D), with:

- the **compiled-in schema** (no editable `items_game.txt` — item data lives in a `.cpp`)
- **full multiplayer networking** — every player sees every other player's unlocks (Kunai example)
- **server-authoritative damage** — attribute bonuses apply on the server, so victims always take
  correct damage (§6 answers this directly)

**This is a plan, not implemented code.** File references are to hlsdk-portable as it stands.

---

## 0. Where things live in hlsdk-portable

| Concern | Location |
|---|---|
| Server game logic | `dlls/` → builds `hl.dll` (or `hl.so`/lib per platform) |
| Client | `cl_dll/` → builds `client.dll` |
| Shared weapon code compiled into both | weapon `.cpp`s listed in both `dlls/CMakeLists.txt` and `cl_dll/CMakeLists.txt` |
| Client weapon prediction | `cl_dll/hl/hl_weapons.cpp` |
| Per-owner networked data structs | `common/entity_state.h` (`clientdata_t.iuser1-4/fuser1-4`), `common/weaponinfo.h` (`weapon_data_t.iuser1-4`) |
| Broadcast-to-everyone entity data | `entity_state_t.iuser1-4/fuser1-4` (`common/entity_state.h:108-115`), filled in `AddToFullPack` (`dlls/client.cpp:1145`) |
| Third-person weapon model | `pev->weaponmodel` (see `dlls/weapons.cpp:984` — `m_pPlayer->pev->weaponmodel = MAKE_STRING( szWeaponModel )`) |
| User message registration | `REG_USER_MSG` calls in `dlls/player.cpp` (LinkUserMessages) |

The build is CMake (`CMakeLists.txt` at root). New shared files must be added to **both**
`dlls/CMakeLists.txt` and `cl_dll/CMakeLists.txt` — same pattern as the existing shared weapon
sources.

---

## 1. The compiled schema

### 1.1 Files

Create three shared files (compiled into BOTH DLLs):

- `dlls/rf_item_schema.h` — types, enums, accessor declarations
- `dlls/rf_item_schema.cpp` — the actual item tables (this *is* our `items_game.txt`)
- `dlls/rf_attributes.h` — attribute hook IDs + `RF_ATTRIB(...)` macros (see §5)

(`dlls/` hosts shared headers in HLSDK convention — cl_dll already includes from there.)

### 1.2 Data model

Keep TF2's shape, trimmed to what GoldSrc needs:

```
ItemAttribute_t  { attrib_id (enum), float value }
RFItemDef_t {
    uint16      defindex            // identity; 0 reserved = "stock/none", 0xFFFF = invalid
    const char *name                // "The Kunai"
    const char *entity_classname    // "weapon_rf_knife" — entity that runs the behavior
    uint8       used_by_classes     // bitmask of RF class enum (fork's own CLASS_* enum)
    uint8       loadout_slot[NUM_CLASSES]  // per-class slot (Pain Train case)
    const char *view_model          // "models/v_kunai.mdl"
    const char *player_model        // "models/p_kunai.mdl"  <- what OTHER players see
    const char *world_model         // "models/w_kunai.mdl"  (dropped/ground)
    uint8       anim_slot           // which third-person anim extension set (ties into m_szAnimExtention system)
    uint8       num_attributes
    ItemAttribute_t attributes[MAX_ITEM_ATTRIBUTES]
}
```

Design rules carried over from TF2:

- **Many defindexes → one classname.** The Kunai is `weapon_rf_knife` with attributes; no new class
  unless the *mechanic* is new (Kunai's health-steal-on-backstab IS a new mechanic, so the knife
  code gets an attribute-gated branch — see §5.3).
- **Attribute IDs are a C enum**, not strings (§5.1). GoldSrc perf + compiled schema means we don't
  need TF2's string matching.
- Values for attributes come from **live TF2's items_game.txt** — copy the numbers, keep mechanics
  matching live TF2.

### 1.3 The table itself

`rf_item_schema.cpp` holds one static const array, indexed lookup built at load:

```cpp
static const RFItemDef_t g_RFItemDefs[] = {
    // defindex, name, classname, classes, ... attributes
    {   0, "Stock",        NULL, ... },                          // sentinel
    { 356, "The Kunai",    "weapon_rf_knife", CLASSBIT(SPY), ...,
        { { RF_ATTRIB_MULT_MAX_HEALTH_ADD, -55.0f },
          { RF_ATTRIB_HEALTH_ON_BACKSTAB,  1.0f } } },
    ...
};
```

Use live TF2 defindexes (356 really is the Kunai) so wiki lookups and schema diffs stay easy.

Accessors (`GetItemDef(defindex)`, `GetItemDefByName`, `GetBaseItemForClassSlot(class, slot)`).
Build a defindex→array-slot lookup table once at `GameDLLInit`/`HUD_Init` — O(1) lookups, zero
per-frame cost. Add `static_assert`/boot-time validation: unique defindexes, valid classnames,
every class+slot has exactly one base item.

**Tamper resistance:** data is inside `hl.dll`/`client.dll`; there is no file to edit. A client
editing *their* DLL only changes their own visuals/prediction — the server's copy decides
everything that matters (§6). Optional dev convenience: an `#ifdef RF_DEV_SCHEMA` text loader for
balance iteration, compiled out of release builds.

---

## 2. Loadout: choosing and storing unlocks

Server-side, per player (`CBasePlayer` members):

- `uint16 m_LoadoutDefindex[NUM_CLASSES][NUM_LOADOUT_SLOTS]` — 0 = stock.
- Set via a client command (`loadout <class> <slot> <defindex>` → handled in
  `ClientCommand`, `dlls/client.cpp`), later via a VGUI/HUD menu that sends the same command.
- Client remembers choices locally (cvars or a config file — this file only stores *which* items
  are picked, not what they do, so it's safe to leave editable).
- Validate server-side on receipt: defindex exists, usable by that class, correct slot. Reject
  otherwise → stock fallback. **Never trust the client's defindex beyond selection.**
- Changes apply on next spawn/resupply (TF2 behavior).

## 3. Spawning weapons from the loadout

Replace hardcoded per-class give-lists with the TF2 flow, in the class spawn/resupply code:

1. For each slot of the player's class: `defindex = m_LoadoutDefindex[class][slot]`
   (0 → `GetBaseItemForClassSlot`).
2. Look up `RFItemDef_t`, get `entity_classname` (run it through a small
   `TranslateWeaponEntForClass`-style table only if we ever share one schema entry across classes
   with different entity needs).
3. If the player already holds a weapon whose `m_iItemDefindex` matches → keep it (TF2's
   `ItemsMatch` resupply behavior, avoids destroy/recreate churn).
4. Else `GiveNamedItem(classname)`, then stamp the new weapon:
   `pWeapon->m_iItemDefindex = defindex` **before** `Deploy` runs, so model/attribute reads
   are correct from the first frame.
5. Remove held weapons whose defindex no longer matches the loadout.

`CBasePlayerWeapon` gains one member: `int m_iItemDefindex` (+ a cached `const RFItemDef_t *`
resolved on set). `Deploy`/`AddToPlayer` read models from the item def instead of hardcoded paths:
viewmodel from `view_model`, and — this is the Kunai-visibility line —
`pev->weaponmodel = MAKE_STRING( def->player_model )` (existing pattern at `dlls/weapons.cpp:984`).

---

## 4. Networking — who needs to know what

Three audiences, three existing GoldSrc channels. No engine changes needed; Xash3D supports all of
this (and honors `delta.lst`, see 4.4).

### 4.1 Everyone in the PVS: what's in that player's hands (the Kunai example)

Two mechanisms, both automatic once set server-side:

- **`pev->weaponmodel`** (p_ model) is part of the player's entity state → every client that can
  see the player renders the Kunai's `p_` model in their hands. Set in `Deploy` from the schema
  (§3). **This alone satisfies "player has Kunai → all other players see the Kunai."**
- **`entity_state_t.iuser1` of the player entity** = active weapon defindex. Set it in
  `AddToFullPack` (`dlls/client.cpp:1145`) when packing a player: read the target player's active
  weapon's `m_iItemDefindex`. Now every client knows *which item* (not just which model) everyone
  is holding — needed for kill feed icons, spy disguises, first-person spectator HUD, and client
  studio-renderer tricks later (attachments, team skins).
  - Check which `iuser` slots the player entity already burns (spectator target etc.) and pick a
    free one; document the allocation in `rf_item_schema.h`.

### 4.2 Full loadout of every player (not just active weapon)

For scoreboard/HUD/bots ("does that Spy have a Dead Ringer equipped?"), add one user message:

- `REG_USER_MSG( "RFLoadout", -1 )` in `LinkUserMessages` (`dlls/player.cpp`, alongside the
  existing 37 registrations).
- Payload: player index (byte), class (byte), then one short per weapon slot. A few bytes per
  player — broadcast (`MSG_ALL`) on spawn/loadout change, and send the full table to late joiners
  from `ClientPutInServer` (same "send state to new client" pattern the spectator `AllowSpec`
  message fix used).
- Client stores it in a `g_PlayerLoadouts[MAX_PLAYERS]` table next to the existing
  `g_PlayerInfoList` usage.

### 4.3 The owner: prediction data

The owning client predicts fire rate/clip locally, so it needs its own weapons' defindexes:

- `weapon_data_t.iuser1` (`common/weaponinfo.h:40`) = defindex, filled in `GetWeaponData`
  (`dlls/client.cpp`), read back in `HUD_WeaponsPostThink` (`cl_dll/hl/hl_weapons.cpp`) and stamped
  onto the client-side predicted weapon instance each frame.
- The client's compiled schema then supplies identical attribute values for prediction — the
  compiled-into-both-DLLs approach *guarantees* client and server agree (TF2 needs file-version
  checks for this; we get it free).
- ⚠ Known footgun (from the fists bug): predicted weapons must be registered in **all three**
  places in `hl_weapons.cpp` or prediction silently breaks. New entity classes added for unlocks
  must follow that checklist.

### 4.4 `delta.lst`

GoldSrc/Xash3D only network the fields listed in the mod's `delta.lst` (game data folder, not the
SDK). Vanilla lists don't necessarily include every `iuser`/`weapon_data` field we use. Step:
verify `entity_state_t.iuser1` and `weapon_data_t.iuser1` are present in our shipped `delta.lst`
(unsigned, 16+ bits) — a field missing there just silently never arrives.

---

## 5. Attributes and gameplay

### 5.1 Hook API

TF2 matches attribute-class *strings* at every hook site with caching. With a compiled schema we
can go simpler and faster — enum IDs and direct summation:

```cpp
// rf_attributes.h
enum RFAttribID_t { RF_ATTRIB_NONE, RF_ATTRIB_MULT_DMG, RF_ATTRIB_MULT_FIRE_RATE,
                    RF_ATTRIB_MULT_CLIP_SIZE, RF_ATTRIB_ADD_MAX_HEALTH, ... };

float RF_AttribHookFloat( float base, RFAttribID_t id, CBasePlayer *pl, CBasePlayerWeapon *wpn );
```

Semantics copied from TF2: multiplicative attributes multiply the base, additive ones add
(`CollateAttributeValues` equivalent — keep it a tiny switch on an is-multiplicative flag in the
attribute's metadata table).

### 5.2 Providers, simplified

TF2's provider graph (weapon → player) exists so *player-level* hooks (move speed, max health)
see attributes on carried items. We replicate the effect cheaply: `RF_AttribHookFloat` with a
player + NULL weapon walks the player's held weapons' cached item defs. Since loadouts only change
on spawn/resupply, **cache per-player aggregate values at loadout-apply time** (e.g.
`m_flAttribMoveSpeedMult`) and just read them in `PM_`/player code — no per-frame walks. This is
the performance-correct GoldSrc translation of TF2's cached provider system.

### 5.3 Hook sites (initial set)

Weapon code (shared, runs in prediction too): damage (`RF_ATTRIB_MULT_DMG` where the weapon
computes its damage value), fire rate (next-attack delay), clip size (on reload/give), reload
speed, spread. Player code (server): max health (Kunai/Eyelander overheal interactions), move
speed, jump behavior. Mechanic-gating attributes (e.g. `RF_ATTRIB_HEALTH_ON_BACKSTAB`) are checked
as booleans/values inside the relevant weapon's existing code path — same pattern as live TF2.

---

## 6. Damage bonuses in multiplayer — the direct answer

**Yes — victims take the correct, attribute-modified damage, automatically.** Here's why, and what
to watch:

1. In GoldSrc, *all* damage runs in the server DLL. `PrimaryAttack` → `FireBulletsPlayer` /
   projectile touch → `TraceAttack`/`TakeDamage` on the victim — every step server-side.
2. The attribute hook at the damage site reads the **server's** compiled schema and the weapon's
   server-side `m_iItemDefindex` (which the server itself assigned in §3 — the client never
   supplies it). Damage bonus weapon → bigger number goes into `TakeDamage`.
3. The victim needs zero knowledge of the attacker's items. They just receive their new health via
   their own `clientdata_t.health`. Nothing about attributes needs to be networked for damage
   correctness.
4. The client-side hook copies (prediction) only affect *feel* (viewmodel timing, predicted clip).
   Even a maliciously edited client DLL cannot change dealt damage — the server recomputes it from
   its own data. This is the same authority model that makes the compiled schema "anti-edit" goal
   actually hold (§1.3).
5. The one thing to keep honest: **muzzle-flash/tracer client events** (`ev_hldm.cpp`) show
   predicted effects; if an unlock changes pellet count/spread, apply the same attribute in the
   shared fire code so visuals match server reality — the shared schema makes the values identical
   by construction.

---

## 7. Step-by-step build order

Each step compiles and is testable on its own; order chosen so multiplayer visibility works early.

1. **Schema core** — add `rf_item_schema.h/.cpp`, `rf_attributes.h` to both CMakeLists. Table with
   stock items + 2–3 test unlocks (Kunai is a good pilot: model swap + attributes + one new
   mechanic). Boot-time validation. No behavior change yet.
2. **Weapon defindex plumbing** — `m_iItemDefindex` on `CBasePlayerWeapon`, stock defaults
   assigned in existing give code, `Deploy` pulls v_/p_ models from schema. *Test: models still
   correct for stock.*
3. **Loadout storage + command** — player command with server-side validation, spawn-from-loadout
   (§3). *Test: `loadout` command swaps knife→Kunai on respawn, viewmodel + p_ model change; second
   client sees the Kunai p_ model.* ← multiplayer visibility milestone
4. **Networking completion** — `GetWeaponData` defindex (owner prediction), `AddToFullPack`
   player iuser (active item broadcast), `RFLoadout` user message + late-join sync, `delta.lst`
   audit. *Test: two clients + demo playback; late joiner sees existing players' items.*
5. **Attribute hooks** — `RF_AttribHookFloat`, damage/fire-rate/clip sites in shared weapon code,
   per-player cached aggregates. *Test: damage-bonus test item; verify victim health loss server-side
   (host + `sv_cheats` gods/impulse 101 harness, or bots — they make perfect test victims).*
6. **Mechanic attributes** — Kunai health-steal etc., gated inside weapon code by attribute
   presence.
7. **HUD/polish** — loadout menu, kill feed icons by defindex, spectator display.

## 8. Pitfalls checklist

- Shared files in **both** CMakeLists, or the DLLs disagree (client falls back to defaults and
  visuals desync).
- `delta.lst` missing fields = silent networking failure (§4.4).
- Predicted weapon triple-registration in `hl_weapons.cpp` (§4.3).
- Stamp `m_iItemDefindex` **before** `Deploy` (else first deploy shows stock model).
- `pev->weaponmodel` must be precached (`PRECACHE_MODEL` every schema model at map start —
  iterate the schema table in `GameDLLInit`/weapon precache; the compiled table makes this a loop,
  not a maintenance list).
- Keep defindex `0` = stock everywhere; never treat it as an error.
- Bots (`CHLBot`): give them loadouts through the same server-side path — they exercise the
  system with zero client involvement, which also proves server authority.
