using System.ComponentModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Views;

/// <summary>One journal card: the entry's text when closed, the editor when open.</summary>
public sealed partial class ReflectionCardView : UserControl
{
    public static readonly DependencyProperty CardProperty = DependencyProperty.Register(
        nameof(Card), typeof(ReflectionCard), typeof(ReflectionCardView),
        new PropertyMetadata(null, OnCardChanged));

    /// <summary>Builds the editor for an opened card; the page supplies it with its dependencies.</summary>
    public static Func<ReflectionCard, UIElement>? EditorFactory { get; set; }

    public ReflectionCardView()
    {
        InitializeComponent();
    }

    public ReflectionCard? Card
    {
        get => (ReflectionCard?)GetValue(CardProperty);
        set => SetValue(CardProperty, value);
    }

    public bool Collapsed => Card is { Expanded: false };

    private static void OnCardChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var view = (ReflectionCardView)d;
        if (e.OldValue is ReflectionCard old)
        {
            old.PropertyChanged -= view.OnCardPropertyChanged;
        }

        if (e.NewValue is ReflectionCard card)
        {
            card.PropertyChanged += view.OnCardPropertyChanged;
        }

        view.SyncEditor();
        view.Bindings.Update();
    }

    private void OnCardPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(ReflectionCard.Editor) or nameof(ReflectionCard.Expanded))
        {
            SyncEditor();
            Bindings.Update();
        }
    }

    private void SyncEditor()
    {
        if (Card is not { Editor: { } editor } card)
        {
            EditorHost.Content = null;
            return;
        }

        TitleBox.Text = editor.Title;
        if (EditorHost.Content is not ReflectionEditorView view || view.ViewModel != editor)
        {
            EditorHost.Content = EditorFactory?.Invoke(card);
        }
    }

    // The header title is the consultation's label, like the sheet's
    private async void OnTitleCommitted(object sender, RoutedEventArgs e)
    {
        if (Card?.Editor is not { } editor)
        {
            return;
        }

        editor.Title = TitleBox.Text;
        await editor.SaveTitleAsync();
        Card.Title = editor.DisplayTitle;
    }
}
