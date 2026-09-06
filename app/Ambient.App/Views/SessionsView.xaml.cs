using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Views;

public sealed partial class SessionsView : UserControl
{
    // Below this width the documents go into tabs
    private const double WideThreshold = 1100;

    private readonly TranscriptPaneView _transcript;
    private readonly NoteEditorView _note;
    private readonly PatientEditorView _patient;
    private bool? _wide;

    public SessionsView(
        SessionsViewModel viewModel, ShellViewModel shell,
        TranscriptPaneView transcript, NoteEditorView note, PatientEditorView patient,
        ConsultationViewModel consultation, Ambient.Client.IEngineClient engine,
        Ambient.App.Core.IUiDispatcher dispatcher, StatusBarViewModel status)
    {
        ViewModel = viewModel;
        Shell = shell;
        _transcript = transcript;
        _note = note;
        _patient = patient;
        InitializeComponent();
        Place(wide: false);
        Loaded += (_, _) =>
        {
            _ = ViewModel.RefreshAsync();
            consultation.OpenReflection = (id, startedAt) =>
                ReflectionSheet.ShowAsync(XamlRoot, engine, dispatcher, status, id, startedAt);
        };
        ViewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(SessionsViewModel.DetailOpen)
                or nameof(SessionsViewModel.EmptyBecauseOff))
            {
                Bindings.Update();
            }
        };
    }

    public SessionsViewModel ViewModel { get; }

    public ShellViewModel Shell { get; }

    public bool SelectHintVisible => !ViewModel.DetailOpen && !ViewModel.EmptyBecauseOff;

    private void OnDetailSizeChanged(object sender, SizeChangedEventArgs e) =>
        Place(e.NewSize.Width >= WideThreshold);

    // The document views are shared with the live screen, so they are moved, not duplicated
    private void Place(bool wide)
    {
        if (_wide == wide)
        {
            return;
        }

        _wide = wide;
        NoteHost.Content = null;
        PatientHost.Content = null;
        TranscriptHost.Content = null;
        NoteHostWide.Content = null;
        PatientHostWide.Content = null;
        TranscriptHostWide.Content = null;
        if (wide)
        {
            NoteHostWide.Content = _note;
            PatientHostWide.Content = _patient;
            TranscriptHostWide.Content = _transcript;
        }
        else
        {
            NoteHost.Content = _note;
            PatientHost.Content = _patient;
            TranscriptHost.Content = _transcript;
        }

        WideLayout.Visibility = wide ? Visibility.Visible : Visibility.Collapsed;
        NarrowLayout.Visibility = wide ? Visibility.Collapsed : Visibility.Visible;
    }

    private void OnPatientFoldClick(object sender, RoutedEventArgs e)
    {
        var open = PatientHostWide.Visibility == Visibility.Visible;
        PatientHostWide.Visibility = open ? Visibility.Collapsed : Visibility.Visible;
        PatientFoldGlyph.Glyph = open ? "" : "";
    }

    // Leaving the page ends the review: edits saved, the engine told
    private async void OnBackClick(object sender, RoutedEventArgs e)
    {
        await ViewModel.LeaveAsync();
        Shell.GoBackCommand.Execute(null);
    }

    // Push the text first: the two-way binding's order against this handler
    // is not guaranteed
    private async void OnTitleCommitted(object sender, RoutedEventArgs e)
    {
        ViewModel.DetailTitle = ((TextBox)sender).Text;
        await ViewModel.RenameAsync();
    }

    // Deletion is crypto-erase, so the confirmation lives here, not in the VM
    private async void OnDeleteClick(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not SessionRow row)
        {
            return;
        }

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Delete this consultation?",
            Content = "The transcript, note and patient sheet are erased and cannot be recovered.",
            PrimaryButtonText = "Delete",
            CloseButtonText = "Keep",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() == ContentDialogResult.Primary)
        {
            await ViewModel.DeleteAsync(row);
        }
    }
}
