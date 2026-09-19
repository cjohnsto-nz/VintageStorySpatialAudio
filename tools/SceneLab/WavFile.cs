namespace SceneLab;

/// <summary>
/// Writes interleaved float samples as a 32-bit IEEE float WAV. More than two channels get
/// WAVE_FORMAT_EXTENSIBLE with the speaker mask of the engine's order (quad, 5.1, 7.1, 7.1.4), so
/// audio tools label the channels correctly.
/// </summary>
public static class WavFile
{
    public static void WriteFloat(string path, ReadOnlySpan<float> interleaved, int channels, int sampleRate)
    {
        using var stream = File.Create(path);
        using var writer = new BinaryWriter(stream);
        int dataBytes = checked(interleaved.Length * sizeof(float));
        bool extensible = channels > 2;
        int formatBytes = extensible ? 40 : 16;
        writer.Write("RIFF"u8);
        writer.Write(4 + (8 + formatBytes) + (8 + dataBytes));
        writer.Write("WAVEfmt "u8);
        writer.Write(formatBytes);
        writer.Write(extensible ? unchecked((short)0xFFFE) : (short)3); // WAVE_FORMAT_EXTENSIBLE / IEEE_FLOAT
        writer.Write((short)channels);
        writer.Write(sampleRate);
        writer.Write(sampleRate * channels * sizeof(float));
        writer.Write((short)(channels * sizeof(float)));
        writer.Write((short)32);
        if (extensible)
        {
            writer.Write((short)22);  // cbSize
            writer.Write((short)32);  // valid bits
            writer.Write(ChannelMask(channels));
            // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00aa00389b71}
            writer.Write(3);
            writer.Write((short)0);
            writer.Write((short)0x10);
            writer.Write([0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71]);
        }

        writer.Write("data"u8);
        writer.Write(dataBytes);
        foreach (float sample in interleaved)
        {
            writer.Write(sample);
        }
    }

    /// <summary>The speaker mask of the engine's channel order for this many channels (0 if none).</summary>
    public static int ChannelMask(int channels) => channels switch
    {
        2 => 0x3,        // FL FR
        4 => 0x33,       // FL FR BL BR
        6 => 0x3F,       // FL FR FC LFE BL BR
        8 => 0x63F,      // + SL SR
        12 => 0x2D63F,   // + TFL TFR TBL TBR
        _ => 0,
    };
}
