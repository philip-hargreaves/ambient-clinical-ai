using Microsoft.UI.Xaml;
using Ambient.App.Core;
using Ambient.App.Core.ViewModels;
using Ambient.Client;

namespace Ambient.App.Views;

/// <summary>Opens the reflection sheet for a consultation, from whichever page shows the note.</summary>
internal static class ReflectionSheet
{
    public static async Task ShowAsync(
        XamlRoot xamlRoot, IEngineClient engine, IUiDispatcher dispatcher, StatusBarViewModel status,
        string sessionId, string startedAt)
    {
        var viewModel = new ReflectionViewModel(engine, dispatcher, status);
        await viewModel.LoadAsync(sessionId, startedAt);
        var dialog = new ReflectionDialog(viewModel, status) { XamlRoot = xamlRoot };
        await dialog.ShowAsync();
    }
}
