using System.Reflection.Emit;
using HarmonyLib;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.Common.Entities;
using Vintagestory.Client.NoObf;

namespace VintageStorySpatialAudio.Takeover;

/// <summary>
/// Harmony patches on the platform's audio seam (docs/PLAN.md §2.1). While a takeover is active
/// every prefix handles the call and skips the original; otherwise they fall through to vanilla.
/// Parameter names must match the originals (Harmony binds by name).
/// </summary>
internal static class PlatformPatches
{
    private static volatile AudioTakeover? active;

    internal static AudioTakeover? Active
    {
        get => active;
        set => active = value;
    }

    // ---- ClientPlatformWindows ----

    /// <summary>Keeps OpenAL closed in-world (CreateAudioData and UpdateAudioListener call it).</summary>
    public static bool StartAudio() => active is null;

    public static bool CreateAudioData(IAsset asset, ref AudioData __result)
    {
        AudioTakeover? takeover = active;
        if (takeover is null)
        {
            return true;
        }

        __result = takeover.DecodeForGame(asset);
        return false;
    }

    public static bool CreateAudio(SoundParams sound, AudioData data, ref ILoadedSound __result)
    {
        AudioTakeover? takeover = active;
        if (takeover is null)
        {
            return true;
        }

        __result = takeover.CreateSound(sound, data)!;
        return false;
    }

    public static bool CreateAudioInGame(SoundParams sound, AudioData data, ref ILoadedSound __result) =>
        CreateAudio(sound, data, ref __result);

    public static bool UpdateAudioListener(float posX, float posY, float posZ, float orientX, float orientY, float orientZ)
    {
        AudioTakeover? takeover = active;
        if (takeover is null)
        {
            return true;
        }

        takeover.OnFrame(posX, posY, posZ, orientX, orientY, orientZ);
        return false;
    }

    public static bool GetAvailableAudioDevices(ref IList<string> __result)
    {
        AudioTakeover? takeover = active;
        if (takeover is null)
        {
            return true;
        }

        __result = takeover.DeviceNames();
        return false;
    }

    public static bool GetCurrentAudioDevice(ref string __result)
    {
        AudioTakeover? takeover = active;
        if (takeover is null)
        {
            return true;
        }

        __result = takeover.CurrentDevice;
        return false;
    }

    public static bool SetCurrentAudioDevice(string value)
    {
        AudioTakeover? takeover = active;
        if (takeover is null)
        {
            return true;
        }

        takeover.SelectDevice(value);
        return false;
    }

    public static bool GetMasterSoundLevel(ref float __result)
    {
        AudioTakeover? takeover = active;
        if (takeover is null)
        {
            return true;
        }

        __result = takeover.MasterLevel;
        return false;
    }

    public static bool SetMasterSoundLevel(float value)
    {
        AudioTakeover? takeover = active;
        if (takeover is null)
        {
            return true;
        }

        takeover.MasterLevel = value;
        return false;
    }

    // ---- LoadedSoundNative ----

    /// <summary>
    /// The device and HRTF settings watchers rebuild the OpenAL context through this; in-world
    /// there is none. The session follows those settings itself.
    /// </summary>
    public static bool ChangeOutputDevice() => active is null;

    // ---- ClientMain ----

    [ThreadStatic]
    private static Entity? emittingEntity;

    /// <summary>The entity a PlaySoundAt(…, Entity, …) overload is playing a sound at, during that call.</summary>
    internal static Entity? EmittingEntity => emittingEntity;

    /// <summary>Prefix on the PlaySoundAt overloads that take an entity: remember it for PlaySoundAtInternal.</summary>
    public static void PlaySoundAtEntity(Entity atEntity, out Entity? __state)
    {
        __state = emittingEntity;
        emittingEntity = atEntity;
    }

    /// <summary>Finalizer for <see cref="PlaySoundAtEntity"/> (runs even if the game throws).</summary>
    public static void PlaySoundAtEntityDone(Entity? __state) => emittingEntity = __state;

    /// <summary>Prefix on PlaySoundAtInternal: a sound that should follow an entity is announced here.</summary>
    public static void PlaySoundAtInternal(AssetLocation? location, double x, double y, double z, EnumSoundType soundType) =>
        active?.OnPlaySound(location, x, y, z, soundType);

    /// <summary>Postfix on PlaySoundAtInternal: 0 means nothing was played.</summary>
    public static void PlaySoundAtInternalDone(double x, double y, double z, int __result)
    {
        if (__result == 0)
        {
            active?.OnPlaySoundSkipped(x, y, z);
        }
    }

    /// <summary>
    /// Removes vanilla's fixed cap of 250 concurrent sounds in PlaySoundAtInternal: the constant
    /// becomes int.MaxValue. Virtualisation in the engine handles load instead.
    /// </summary>
    /// <para>
    /// Also widens vanilla's range check there (a sound further away than its range is never
    /// created): <c>distance² &gt; range * range</c> becomes <c>distance² &gt; range * range * scale²</c>.
    /// With physical fall-off a sound fades with distance rather than vanishing at its range.
    /// </para>
    public static IEnumerable<CodeInstruction> RemoveSoundCap(IEnumerable<CodeInstruction> instructions)
    {
        ArgumentNullException.ThrowIfNull(instructions);
        CodeInstruction[] code = [.. instructions];
        int replaced = 0;
        int scaled = 0;
        var result = new List<CodeInstruction>(code.Length + 2);
        for (int i = 0; i < code.Length; i++)
        {
            CodeInstruction instruction = code[i];
            if (replaced == 0 && instruction.opcode == OpCodes.Ldc_I4 && instruction.operand is int value && value == VanillaSoundCap)
            {
                instruction.operand = int.MaxValue;
                replaced++;
            }

            result.Add(instruction);
            // range * range: ldarg range, ldarg range, mul.
            if (scaled == 0 && i >= 2 && instruction.opcode == OpCodes.Mul
                && code[i - 1].IsLdarg(RangeArgument) && code[i - 2].IsLdarg(RangeArgument))
            {
                result.Add(new CodeInstruction(OpCodes.Ldsfld, AccessTools.Field(typeof(PlatformPatches), nameof(RangeScaleSquared))));
                result.Add(new CodeInstruction(OpCodes.Mul));
                scaled++;
            }
        }

        SoundCapRemoved = replaced == 1;
        RangeWidened = scaled == 1;
        return result;
    }

    /// <summary>PlaySoundAtInternal's range parameter (argument 0 is the instance).</summary>
    internal const int RangeArgument = 7;

    /// <summary>The range multiplier, squared (the check compares squared distances).</summary>
    public static float RangeScaleSquared = 9f;

    /// <summary>Whether the last <see cref="RemoveSoundCap"/> found and widened the range check.</summary>
    internal static bool RangeWidened { get; private set; }

    internal const int VanillaSoundCap = 250;

    /// <summary>Whether the last <see cref="RemoveSoundCap"/> found and replaced the cap.</summary>
    internal static bool SoundCapRemoved { get; private set; }

    // ---- AnimationManager ----

    /// <summary>
    /// Prefix on AnimationManager.OnClientFrame: advances the animations of a creature that is not
    /// being drawn, so the footsteps and other sounds its animation frames trigger still play
    /// (ADR 0020). Vanilla skips the animator altogether unless the creature is on screen, in the
    /// shadow pass or dead, so a wolf behind you runs silently. Always falls through to the
    /// original, which then does its own (skipped) work and its looping-sound housekeeping.
    /// </summary>
    public static void AnimationOnClientFrame(AnimationManager __instance, Entity? ___entity, float dt) =>
        active?.OnUnseenAnimationFrame(__instance, ___entity, dt);
}
