using System.Collections.ObjectModel;
using System.Globalization;
using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Ambient.Client;

namespace Ambient.App.Core.ViewModels;

/// <summary>One consultation the clinician reflected on: a journal card.</summary>
public sealed partial class ReflectionCard : ObservableObject
{
    public ReflectionCard(string id, string title, DateTimeOffset started, string learned,
        string summary = "", bool demo = false, string happened = "", string next = "")
    {
        Id = id;
        Title = title;
        Started = started;
        Learned = learned;
        Summary = summary;
        Demo = demo;
        Happened = happened;
        Next = next;
    }

    /// <summary>Searched, not shown.</summary>
    [ObservableProperty]
    public partial string Happened { get; set; }

    [ObservableProperty]
    public partial string Next { get; set; }

    /// <summary>Everything a search may match: title, case study and the three answers.</summary>
    public bool Matches(string needle) =>
        Title.Contains(needle, StringComparison.CurrentCultureIgnoreCase)
        || Summary.Contains(needle, StringComparison.CurrentCultureIgnoreCase)
        || Happened.Contains(needle, StringComparison.CurrentCultureIgnoreCase)
        || Learned.Contains(needle, StringComparison.CurrentCultureIgnoreCase)
        || Next.Contains(needle, StringComparison.CurrentCultureIgnoreCase);

    public string Id { get; }

    /// <summary>A seeded sample.</summary>
    public bool Demo { get; }

    [ObservableProperty]
    public partial string Title { get; set; }

    public DateTimeOffset Started { get; }

    public string MonthLabel => Started.ToString("MMMM yyyy", CultureInfo.CurrentCulture);

    /// <summary>Searched, not shown.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Line))]
    public partial string Learned { get; set; }

    /// <summary>What the closed card shows: the case study, or the clinician's words until there is one.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Line))]
    public partial string Summary { get; set; }

    public string Line => (Summary.Trim().Length > 0 ? Summary : Learned).Trim();

    /// <summary>True on the first card of each month, so the month heads the group.</summary>
    [ObservableProperty]
    public partial bool StartsMonth { get; set; }

    [ObservableProperty]
    public partial bool Expanded { get; set; }

    /// <summary>The editor, present only while the card is open.</summary>
    [ObservableProperty]
    public partial ReflectionViewModel? Editor { get; set; }

    /// <summary>Set by the page, which owns the engine.</summary>
    public Func<ReflectionCard, Task>? ToggleRequested { get; set; }

    public Func<ReflectionCard, Task>? DeleteRequested { get; set; }

    [RelayCommand]
    private Task Toggle() => ToggleRequested?.Invoke(this) ?? Task.CompletedTask;

    [RelayCommand]
    private Task Delete() => DeleteRequested?.Invoke(this) ?? Task.CompletedTask;
}

/// <summary>One month of the year strip: its number, short name, how many entries it holds, and whether it is this month.</summary>
public sealed record MonthMarker(int Month, string Name, int Count, bool Current = false, bool Selected = false)
{
    public bool Filled => Count > 0;
}

/// <summary>
/// The Appraisal page: a journal of reflections, one year at a time.
/// </summary>
public sealed partial class AppraisalsViewModel : ObservableObject
{
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(5);

    private readonly IEngineClient _engine;
    private readonly IUiDispatcher _dispatcher;
    private readonly StatusBarViewModel _status;
    private readonly List<ReflectionCard> _all = [];

    public AppraisalsViewModel(IEngineClient engine, IUiDispatcher dispatcher, StatusBarViewModel status)
    {
        _engine = engine;
        _dispatcher = dispatcher;
        _status = status;
    }

    public ObservableCollection<ReflectionCard> Cards { get; } = [];

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(YearLabel))]
    [NotifyCanExecuteChangedFor(nameof(PreviousYearCommand))]
    [NotifyCanExecuteChangedFor(nameof(NextYearCommand))]
    public partial int Year { get; private set; }

    public string YearLabel => Year == 0 ? "" : Year.ToString(CultureInfo.InvariantCulture);

    [ObservableProperty]
    public partial string Search { get; set; } = "";

    [ObservableProperty]
    public partial string CountLabel { get; private set; } = "";

    /// <summary>No entries at all: the page shows its empty state.</summary>
    [ObservableProperty]
    public partial bool Empty { get; private set; } = true;

    [ObservableProperty]
    public partial IReadOnlyList<MonthMarker> Months { get; private set; } = Array.Empty<MonthMarker>();

    partial void OnSearchChanged(string value) => Rebuild();

    /// <summary>The month the list is narrowed to, 0 for the whole year.</summary>
    [ObservableProperty]
    public partial int MonthFilter { get; private set; }

    /// <summary>Pressing the year shows all of it.</summary>
    [RelayCommand]
    private void ShowWholeYear()
    {
        if (MonthFilter != 0)
        {
            MonthFilter = 0;
            Rebuild();
        }
    }

    /// <summary>Pressing a month narrows the year to it; pressing it again shows the year.</summary>
    public void ToggleMonth(int month)
    {
        if (_all.All(c => c.Started.Year != Year || c.Started.Month != month))
        {
            return;
        }

        MonthFilter = MonthFilter == month ? 0 : month;
        Rebuild();
    }

    [RelayCommand]
    public async Task RefreshAsync()
    {
        await CollapseAllAsync().ConfigureAwait(true);
        try
        {
            var result = await _engine.RequestAsync("reflection/list", null, RequestTimeout)
                .ConfigureAwait(true);
            _all.Clear();
            foreach (var entry in result.GetProperty("reflections").EnumerateArray())
            {
                var started = DateTimeOffset.TryParse(
                    Text(entry, "startedAt"), CultureInfo.InvariantCulture, out var when)
                    ? when.ToLocalTime()
                    : DateTimeOffset.Now;
                var label = Text(entry, "label");
                _all.Add(new ReflectionCard(
                    Text(entry, "id"),
                    label.Length > 0 ? label : started.ToString("d MMMM", CultureInfo.CurrentCulture),
                    started,
                    Text(entry, "learned"),
                    Text(entry, "summary"),
                    entry.TryGetProperty("demo", out var demo) && demo.ValueKind == JsonValueKind.True,
                    Text(entry, "happened"),
                    Text(entry, "next"))
                {
                    ToggleRequested = ToggleAsync,
                    DeleteRequested = DeleteAsync,
                });
            }

            if (_all.Count > 0 && _all.All(c => c.Started.Year != Year))
            {
                Year = _all.Max(c => c.Started.Year);
            }
            else if (_all.Count == 0)
            {
                Year = DateTimeOffset.Now.Year;
            }

            Rebuild();
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            _status.Append($"could not list reflections: {e.Message}");
        }
    }

    private void Rebuild()
    {
        var needle = Search.Trim();
        var shown = _all
            .Where(c => c.Started.Year == Year)
            .Where(c => MonthFilter == 0 || c.Started.Month == MonthFilter)
            .Where(c => needle.Length == 0 || c.Matches(needle))
            .OrderByDescending(c => c.Started)
            .ToList();
        var lastMonth = -1;
        foreach (var card in shown)
        {
            card.StartsMonth = card.Started.Month != lastMonth;
            lastMonth = card.Started.Month;
        }

        Cards.Clear();
        foreach (var card in shown)
        {
            Cards.Add(card);
        }

        var inYear = _all.Count(c => c.Started.Year == Year);
        var today = DateTimeOffset.Now;
        Months = Enumerable.Range(1, 12)
            .Select(m => new MonthMarker(
                m,
                CultureInfo.CurrentCulture.DateTimeFormat.GetAbbreviatedMonthName(m),
                _all.Count(c => c.Started.Year == Year && c.Started.Month == m),
                Year == today.Year && m == today.Month,
                MonthFilter == m))
            .ToList();
        CountLabel = inYear == 1 ? "1 reflection" : $"{inYear} reflections";
        Empty = _all.Count == 0;
    }

    private bool CanGoToPreviousYear() => _all.Any(c => c.Started.Year < Year);

    private bool CanGoToNextYear() => _all.Any(c => c.Started.Year > Year);

    [RelayCommand(CanExecute = nameof(CanGoToPreviousYear))]
    private async Task PreviousYear()
    {
        await CollapseAllAsync().ConfigureAwait(true);
        Year = _all.Where(c => c.Started.Year < Year).Max(c => c.Started.Year);
        MonthFilter = 0;
        Rebuild();
    }

    [RelayCommand(CanExecute = nameof(CanGoToNextYear))]
    private async Task NextYear()
    {
        await CollapseAllAsync().ConfigureAwait(true);
        Year = _all.Where(c => c.Started.Year > Year).Min(c => c.Started.Year);
        MonthFilter = 0;
        Rebuild();
    }

    /// <summary>"March 2026": the month in the year shown.</summary>
    public string MonthName(int month) =>
        new DateTimeOffset(Year, month, 1, 0, 0, 0, TimeSpan.Zero).ToString("MMMM yyyy", CultureInfo.CurrentCulture);

    /// <summary>Opens one card, closing (and saving) any other.</summary>
    public async Task ToggleAsync(ReflectionCard card)
    {
        if (card.Expanded)
        {
            await CollapseAsync(card).ConfigureAwait(true);
            return;
        }

        await CollapseAllAsync().ConfigureAwait(true);
        var editor = new ReflectionViewModel(_engine, _dispatcher, _status);
        await editor.LoadAsync(card.Id, card.Started.ToString("o", CultureInfo.InvariantCulture))
            .ConfigureAwait(true);
        card.Editor = editor;
        card.Expanded = true;
    }

    private static async Task CollapseAsync(ReflectionCard card)
    {
        if (card.Editor is { } editor)
        {
            await editor.SaveAsync().ConfigureAwait(true);
            await editor.SaveTitleAsync().ConfigureAwait(true);
            card.Title = editor.DisplayTitle;
            card.Happened = editor.Happened;
            card.Learned = editor.Learned;
            card.Next = editor.Next;
            card.Summary = editor.Summary;
            editor.Detach();
        }

        card.Editor = null;
        card.Expanded = false;
    }

    private async Task CollapseAllAsync()
    {
        foreach (var card in _all.Where(c => c.Expanded).ToList())
        {
            await CollapseAsync(card).ConfigureAwait(true);
        }

        Rebuild();
    }

    public async Task DeleteAsync(ReflectionCard card)
    {
        try
        {
            card.Editor?.Detach();
            card.Editor = null;
            card.Expanded = false;
            _ = await _engine.RequestAsync("reflection/delete", new { id = card.Id }, RequestTimeout)
                .ConfigureAwait(true);
            _all.Remove(card);
            Rebuild();
            _status.Append("Reflection removed");
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            _status.Append($"could not remove the reflection: {e.Message}");
        }
    }

    /// <summary>Leaving the page saves whatever is open.</summary>
    public Task LeaveAsync() => CollapseAllAsync();

    private static string Text(JsonElement element, string property) =>
        element.ValueKind == JsonValueKind.Object
            && element.TryGetProperty(property, out var value)
            && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? ""
            : "";
}
