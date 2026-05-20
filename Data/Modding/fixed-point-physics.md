# Fixed-point terrain physics (MP M3) — what modders need to know

**Short version: nothing you do changes. Terrain destruction just became
reproducible.**

MP M3 rewrote the terrain-destruction math — how an impulse decides whether a
terrain pixel is knocked loose, and the geometry of carve/dislodge operations —
in integer fixed-point instead of floating point. Integer arithmetic is exactly
specified by the C++ standard, so the same carve now produces the *same result
on every compiler and operating system*. This is groundwork for deterministic
multiplayer: a co-op session replicates terrain edits as "carve here" commands,
and every client must apply them bit-identically.

## What is unchanged

- **`Material` properties** — `Integrity`, `Restitution`, `Friction`,
  `StructuralIntegrity`, densities — are still floats in INI and still read as
  floats from Lua. Nothing in your `.rte` files changes.
- **`Vector`** — actor/particle `Pos` and `Vel`, and every Lua vector — is
  still a float type. `actor.Vel`, `actor.Pos`, `Vector(x, y)` behave exactly
  as before.
- **The Lua API** — every binding, every property, every script hook — is
  identical. No script needs editing.
- **The INI format** — unchanged. A mod authored before M3 loads and plays
  after M3 with no migration.

The fixed-point math lives entirely inside the engine's destruction functions
(`WillPenetrate` / `TryPenetrate` / `DislodgePixel`, the dislodge-circle/ring/
box/line helpers, and the death-silhouette carve). Float values are converted
to fixed-point at the function boundary and back on the way out.

## What you might notice

- **Reproducibility.** Run the same scenario from the same seed and terrain is
  carved identically — across machines, not just on yours.
- **Sub-pixel precision differences vs. earlier versions.** Fixed-point has a
  precision of ~6×10⁻⁸ — far finer than one pixel. At the exact edge of a carve
  a single pixel may occasionally fall on the other side of the threshold
  compared to a pre-M3 build. This is below the visible threshold; destruction
  looks the same.
- **No performance cost.** Integer fixed-point destruction is as fast as the
  float path or faster (no FPU transcendentals on the hot path).

## What else changed

The atom **collision response** — how objects bounce, slide and settle — is
also fixed-point now (the impulse, restitution, friction and moment-of-inertia
math). As with terrain destruction: the `Material` / `Vector` / INI / Lua API
are unchanged, and the only effect is that bouncing and settling are
reproducible across machines.

## Related

- `Source/Network/README.md` — the determinism scaffold and the M3 fixed-point
  section, for engine contributors.
- `Data/Modding/lua-determinism.md` — the MP M2 Lua determinism contract.
