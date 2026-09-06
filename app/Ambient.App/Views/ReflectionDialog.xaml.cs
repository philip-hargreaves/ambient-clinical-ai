using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Views;

/// <summary>
/// The reflection sheet over the review screen. Closing saves changes.
/// </summary>
public sealed partial class ReflectionDialog : ContentDialog
{
    public ReflectionDialog(ReflectionViewModel viewModel, StatusBarViewModel status)
    {
        ViewModel = viewModel;
        InitializeComponent();
        EditorHost.Content = new ReflectionEditorView(viewModel, status);
    }

    public ReflectionViewModel ViewModel { get; }

    private void OnClosing(ContentDialog sender, ContentDialogClosingEventArgs args)
    {
        // The deferral keeps the dialog until the save has gone to the engine
        var deferral = args.GetDeferral();
        _ = FinishAsync(deferral);
    }

    private async Task FinishAsync(ContentDialogClosingDeferral deferral)
    {
        try
        {
            await ViewModel.SaveAsync();
            await ViewModel.SaveTitleAsync();
        }
        finally
        {
            ViewModel.Detach();
            deferral.Complete();
        }
    }
}
