namespace SceneLab;

/// <summary>Writes interleaved float samples as a 32-bit IEEE float WAV.</summary>
public static class WavFile
{
    public static void WriteFloat(string path, ReadOnlySpan<float> interleaved, int channels, int sampleRate)
    {
        using var stream = File.Create(path);
        using var writer = new BinaryWriter(stream);
        int dataBytes = checked(interleaved.Length * sizeof(float));
        writer.Write("RIFF"u8);
        writer.Write(36 + dataBytes);
        writer.Write("WAVEfmt "u8);
        writer.Write(16);
        writer.Write((short)3); // WAVE_FORMAT_IEEE_FLOAT
        writer.Write((short)channels);
        writer.Write(sampleRate);
        writer.Write(sampleRate * channels * sizeof(float));
        writer.Write((short)(channels * sizeof(float)));
        writer.Write((short)32);
        writer.Write("data"u8);
        writer.Write(dataBytes);
        foreach (float sample in interleaved)
        {
            writer.Write(sample);
        }
    }
}
