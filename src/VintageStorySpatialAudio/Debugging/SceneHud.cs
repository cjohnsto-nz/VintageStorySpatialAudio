using Vintagestory.API.Client;

namespace VintageStorySpatialAudio.Debugging;

/// <summary>
/// A live text panel (top left) for the scene overlay and the sound inspector, refreshed by its
/// owner. It grows with what it is given, up to the height of the window: a panel that silently
/// cuts off the lines it cannot fit is worse than no panel, because you cannot tell that it did.
/// </summary>
internal sealed class SceneHud : HudElement
{
    private const string TextKey = "text";
    private const double Width = 860.0;
    private const double LineHeight = 19.0;
    private const double MinLines = 3.0;

    private readonly ICoreClientAPI api;
    private int composedLines = -1;
    private string text = string.Empty;

    public SceneHud(ICoreClientAPI capi)
        : base(capi)
    {
        api = capi;
        Compose(12);
    }

    public override string? ToggleKeyCombinationCode => null;

    public override bool Focusable => false;

    /// <summary>The lines the panel can show at the window's current height.</summary>
    public int LineBudget => (int)((api.Render.FrameHeight - 140) / LineHeight);

    public void SetText(string value)
    {
        text = value ?? string.Empty;
        int lines = 1;
        foreach (char c in text)
        {
            lines += c == '\n' ? 1 : 0;
        }

        if (lines != composedLines)
        {
            Compose(lines);
        }

        SingleComposer.GetDynamicText(TextKey).SetNewText(text);
    }

    private void Compose(int lines)
    {
        double height = Math.Clamp(lines, MinLines, LineBudget) * LineHeight;
        ElementBounds textBounds = ElementBounds.Fixed(EnumDialogArea.None, 0, 0, Width, height);
        ElementBounds background = textBounds.ForkBoundingParent(6, 6, 6, 6);
        ElementBounds dialog = ElementStdBounds.AutosizedMainDialog
            .WithAlignment(EnumDialogArea.LeftTop)
            .WithFixedAlignmentOffset(GuiStyle.DialogToScreenPadding, GuiStyle.DialogToScreenPadding + 40);
        SingleComposer?.Dispose();
        SingleComposer = api.Gui.CreateCompo("spatialaudio-scene-hud", dialog)
            .AddGameOverlay(background)
            .AddDynamicText(text, CairoFont.WhiteSmallText(), textBounds, TextKey)
            .Compose();
        composedLines = lines;
    }
}
