namespace VintageStorySpatialAudio.Native;

/// <summary>A failure reported by the native engine.</summary>
public sealed class NativeException : Exception
{
    internal NativeException(string operation, VsaResult result, string nativeMessage)
        : base($"{operation} failed ({result}): {(string.IsNullOrEmpty(nativeMessage) ? "no details" : nativeMessage)}")
    {
        Result = result.ToString();
    }

    /// <summary>The native result code name.</summary>
    public string Result { get; }

    internal static void ThrowIfFailed(VsaResult result, string operation)
    {
        if (result != VsaResult.Ok)
        {
            // Read immediately: the message is thread-local and overwritten by the next call.
            throw new NativeException(operation, result, VsaNative.GetLastError());
        }
    }
}
