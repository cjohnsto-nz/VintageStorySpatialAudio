# ADR 0020: A creature you cannot see still makes footsteps

**Status:** Accepted, 21 Sep 2026.

## Context

A creature's footsteps are not played by anything that knows about walking. They hang off the
frames of its animations: `wolf-adult.json` puts four `animationSounds` on `Walk` (frames 2, 13,
30 and 42) and four more on each of `Run` and `Canter`; the bear does the same. The client's
`ClientAnimator.OnFrame` fires them as the animation passes those frames.

`AnimationManager.OnClientFrame` only calls the animator when the creature is being drawn:

```csharp
if (entity.IsRendered || entity.IsShadowRendered || !entity.Alive)
{
    Animator.OnFrame(ActiveAnimationsByAnimCode, dt);
    ...
}
```

`IsRendered` is decided every frame in `SystemRenderEntities.OnBeforeRender`: in the frustum, in
the player's dimension, and within the view distance in a rendered chunk. So a wolf **behind you**
is not drawn, its animation does not advance, and it runs in silence. It is audible only while it
is on screen — which is the one time you do not need to be told it is there.

This mod makes the loss worse rather than better. Fall-off is physical and ranges are widened
(`SoundRangeMultiplier`, default 3), so the wolf's 10–15 m footsteps should now carry much
further; none of that matters while the sound is never played at all.

## Decision

A prefix on `AnimationManager.OnClientFrame` advances the animations of the creatures vanilla is
about to skip, so their frames still trigger their sounds. It runs only when all of these hold:

- the creature is not drawn this frame, and is alive — otherwise vanilla advances it itself and
  the prefix does nothing;
- the game is not paused (as vanilla checks);
- one of its active animations carries a sound with a location — which is nearly no creature at
  all, and is the check that keeps this cheap;
- the listener is within that sound's range times `SoundRangeMultiplier` — the same distance at
  which `PlaySoundAtInternal` would refuse to start it.

`IAnimator.CalculateMatrices` is turned off around the call and restored afterwards. The matrices
are the expensive part of a frame and exist only to pose the model for the shader; nothing is
drawing this creature. What remains is advancing the running animations and firing their sounds.

The sound itself is unchanged: `AnimationManager.ShouldPlaySound` plays it at the creature's
current position through the plain-coordinates `PlaySoundAt`, and the entity-sound inference
already matches it to the creature and makes it follow (see the Phase 8 entity tracking).

Settable as `SoundsFromUnseenCreatures` (default on), and measured as the `unseen animation`
section of `.spatialaudio perf`.

## Consequences

- You hear a wolf coming up behind you. Footsteps, and any other animation-frame sound, now carry
  as far as the rest of the mix does.
- `AnimationManager.OnClientFrame` is **not** in the takeover's foreign-patch check, unlike every
  other method it patches. Animation mods patch it, our prefix only adds work and always falls
  through to the original, and there is no reason to hand audio back to vanilla over it.
- An unseen creature's animation phase now advances, where vanilla froze it. Its poses do not, so
  the first frame it is drawn again computes them as before. Nothing reads a pose of a creature
  that is not being drawn.
- The cost is one `AnimatorBase.OnFrame` per off-screen creature within earshot whose animations
  carry sounds. On the dictionary of active animations that is a handful of creatures at most;
  everything else is turned away by the two cheap checks before it.
- It is part of the takeover, like every other hook: with `TakeOverGameAudio` off, or when the
  integration points fail to verify, vanilla audio stays in charge and so does its silence.
- `GameIntegrationTests` asserts that the installed game's `OnClientFrame` still reads
  `Entity.IsRendered` and `Entity.IsShadowRendered`. If a game update fixes this itself the test
  fails, and the patch and its setting should go.
