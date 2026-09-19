using Vintagestory.API.Client;

namespace VintageStorySteamAudio.Debugging;

/// <summary>A small live text panel (top left) for the scene overlay; refreshed by its owner.</summary>
internal sealed class SceneHud : HudElement
{
    private const string TextKey = "text";

    public SceneHud(ICoreClientAPI capi)
        : base(capi)
    {
        ElementBounds text = ElementBounds.Fixed(EnumDialogArea.None, 0, 0, 560, 150);
        ElementBounds background = text.ForkBoundingParent(6, 6, 6, 6);
        ElementBounds dialog = ElementStdBounds.AutosizedMainDialog
            .WithAlignment(EnumDialogArea.LeftTop)
            .WithFixedAlignmentOffset(GuiStyle.DialogToScreenPadding, GuiStyle.DialogToScreenPadding + 40);
        SingleComposer = capi.Gui.CreateCompo("vssteamaudio-scene-hud", dialog)
            .AddGameOverlay(background)
            .AddDynamicText(string.Empty, CairoFont.WhiteSmallText(), text, TextKey)
            .Compose();
    }

    public override string? ToggleKeyCombinationCode => null;

    public override bool Focusable => false;

    public void SetText(string text) => SingleComposer.GetDynamicText(TextKey).SetNewText(text);
}
