using System.Reflection.Emit;
using HarmonyLib;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.Client.NoObf;

namespace VintageStorySteamAudio.Takeover;

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

    /// <summary>
    /// Removes vanilla's fixed cap of 250 concurrent sounds in PlaySoundAtInternal: the constant
    /// becomes int.MaxValue. Virtualisation in the engine handles load instead.
    /// </summary>
    public static IEnumerable<CodeInstruction> RemoveSoundCap(IEnumerable<CodeInstruction> instructions)
    {
        ArgumentNullException.ThrowIfNull(instructions);
        int replaced = 0;
        foreach (CodeInstruction instruction in instructions)
        {
            if (replaced == 0 && instruction.opcode == OpCodes.Ldc_I4 && instruction.operand is int value && value == VanillaSoundCap)
            {
                instruction.operand = int.MaxValue;
                replaced++;
            }

            yield return instruction;
        }

        SoundCapRemoved = replaced == 1;
    }

    internal const int VanillaSoundCap = 250;

    /// <summary>Whether the last <see cref="RemoveSoundCap"/> found and replaced the cap.</summary>
    internal static bool SoundCapRemoved { get; private set; }
}
