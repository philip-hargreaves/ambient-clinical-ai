using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core.Appraisal;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Views;

/// <summary>
/// Saves one reflection as text; warns about identifiers first.
/// </summary>
internal static class ReflectionSave
{
    public static async Task SaveAsync(XamlRoot root, StatusBarViewModel status, string text, string title)
    {
        var warning = IdentifierCheck.Describe(text);
        if (warning.Length > 0)
        {
            var check = new ContentDialog
            {
                XamlRoot = root,
                Title = "Check before saving",
                Content = warning + "\n\nChange the wording, or save as it is.",
                PrimaryButtonText = "Save anyway",
                CloseButtonText = "Go back",
                DefaultButton = ContentDialogButton.Close,
            };
            if (await check.ShowAsync() != ContentDialogResult.Primary)
            {
                return;
            }
        }

        var path = await SavePickerHelper.PickAsync(FileName(title), "Plain text", ".txt");
        if (path is null)
        {
            return;
        }

        await File.WriteAllTextAsync(path, text);
        status.Append($"Reflection saved to {Path.GetFileName(path)}");
    }

    private static string FileName(string title)
    {
        var invalid = Path.GetInvalidFileNameChars();
        var name = new string(title.Select(c => invalid.Contains(c) ? ' ' : c).ToArray()).Trim();
        return name.Length > 0 ? "reflection - " + name : "reflection";
    }
}
