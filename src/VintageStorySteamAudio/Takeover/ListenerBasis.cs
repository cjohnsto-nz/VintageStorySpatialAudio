namespace VintageStorySteamAudio.Takeover;

/// <summary>
/// Listener orientation from a view direction. Vanilla hands OpenAL a flattened forward vector
/// (y = 0), which loses looking up and down; we use the player's full view vector and derive the
/// matching up vector, so sources above and below stay above and below.
/// </summary>
public sealed class ListenerBasis
{
    private float headingX;
    private float headingZ = -1f;

    public float ForwardX { get; private set; }

    public float ForwardY { get; private set; }

    public float ForwardZ { get; private set; } = -1f;

    public float UpX { get; private set; }

    public float UpY { get; private set; } = 1f;

    public float UpZ { get; private set; }

    /// <summary>Unit horizontal direction the listener faces (kept while looking straight up or down).</summary>
    public float HeadingX => headingX;

    public float HeadingZ => headingZ;

    /// <summary>Updates from a view direction (any length). Returns false and keeps the previous basis if it is degenerate.</summary>
    public bool Update(float x, float y, float z)
    {
        float length = MathF.Sqrt((x * x) + (y * y) + (z * z));
        if (!float.IsFinite(length) || length < 1e-6f)
        {
            return false;
        }

        x /= length;
        y /= length;
        z /= length;

        // Heading: the horizontal part of the view. Looking straight up or down it vanishes, so
        // keep the last heading (the head has not turned, only tilted).
        float horizontal = MathF.Sqrt((x * x) + (z * z));
        if (horizontal > 1e-3f)
        {
            headingX = x / horizontal;
            headingZ = z / horizontal;
        }

        // Pitch t: forward = (cos t * h, sin t, ...), up = (-sin t * h, cos t, ...).
        float sinPitch = Math.Clamp(y, -1f, 1f);
        float cosPitch = MathF.Sqrt(1f - (sinPitch * sinPitch));
        ForwardX = cosPitch * headingX;
        ForwardY = sinPitch;
        ForwardZ = cosPitch * headingZ;
        UpX = -sinPitch * headingX;
        UpY = cosPitch;
        UpZ = -sinPitch * headingZ;
        return true;
    }
}
