using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI.Text;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Views;

public sealed partial class MonthMarkerView : UserControl
{
    public static readonly DependencyProperty MarkerProperty = DependencyProperty.Register(
        nameof(Marker), typeof(MonthMarker), typeof(MonthMarkerView),
        new PropertyMetadata(null, (d, _) => ((MonthMarkerView)d).Bindings.Update()));

    /// <summary>Set by the page: takes the month pressed, whether it has entries or not.</summary>
    public static Action<MonthMarker>? Pressed { get; set; }

    public MonthMarkerView()
    {
        InitializeComponent();
    }

    public MonthMarker? Marker
    {
        get => (MonthMarker?)GetValue(MarkerProperty);
        set => SetValue(MarkerProperty, value);
    }

    public bool Filled => Marker is { Filled: true };

    public bool Selected => Marker is { Selected: true };

    // Divider before every cell but the first
    public Thickness Divider => Marker is { Month: 1 } ? new Thickness(0) : new Thickness(1, 0, 0, 0);

    // Subtle fill when selected
    public Brush Fill => Marker is { Selected: true }
        ? (Brush)Application.Current.Resources["SubtleFillColorSecondaryBrush"]
        : new SolidColorBrush(Microsoft.UI.Colors.Transparent);

    // Months with entries and this month in ink, the rest faint
    public Brush Label => Marker is { Filled: true } or { Current: true }
        ? (Brush)Application.Current.Resources["TextFillColorPrimaryBrush"]
        : (Brush)Application.Current.Resources["FaintTextBrush"];

    public FontWeight Weight => Marker is { Current: true } ? FontWeights.SemiBold : FontWeights.Normal;

    public string Tip => Marker is null
        ? ""
        : Marker.Count == 0 ? "No reflections"
        : Marker.Selected ? "Show the whole year"
        : Marker.Count == 1 ? "1 reflection, press to show only this month"
        : $"{Marker.Count} reflections, press to show only this month";

    private void OnClick(object sender, RoutedEventArgs e)
    {
        if (Marker is { } marker)
        {
            Pressed?.Invoke(marker);
        }
    }
}
