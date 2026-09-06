using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

public class AppraisalsViewModelTest
{
    private static (AppraisalsViewModel Page, FakeEngineClient Engine) Create()
    {
        var engine = new FakeEngineClient();
        engine.Reflections.Add(("a", "2026-09-04T09:12:00Z", "Elbow swelling", "check the temperature\nand more", "A patient in their forties."));
        engine.Reflections.Add(("b", "2026-09-01T14:00:00Z", "Cough", "ask about smoking", ""));
        engine.Reflections.Add(("c", "2026-06-20T10:00:00Z", "Back pain", "", "A patient in their sixties with back pain."));
        engine.Reflections.Add(("d", "2025-11-03T10:00:00Z", "", "listen longer", ""));
        engine.DemoReflections.Add("c");
        return (new AppraisalsViewModel(engine, new InlineDispatcher(), new StatusBarViewModel()), engine);
    }

    [Fact]
    public async Task TheJournalShowsTheLatestYearNewestFirstWithMonthHeadings()
    {
        var (page, _) = Create();

        await page.RefreshAsync();

        Assert.Equal(2026, page.Year);
        Assert.Equal("3 reflections", page.CountLabel);
        Assert.False(page.Empty);
        Assert.Equal(["a", "b", "c"], page.Cards.Select(c => c.Id));
        Assert.Equal([true, false, true], page.Cards.Select(c => c.StartsMonth));
        Assert.Equal("A patient in their forties.", page.Cards[0].Line);
        Assert.Equal("ask about smoking", page.Cards[1].Line);  // no case study yet: the clinician's words
        Assert.Equal("A patient in their sixties with back pain.", page.Cards[2].Line);
        Assert.Equal([false, false, true], page.Cards.Select(c => c.Demo));
        Assert.Equal("September 2026", page.Cards[0].MonthLabel);
        Assert.Equal(12, page.Months.Count);
        Assert.Equal(2, page.Months[8].Count);  // September
        Assert.True(page.Months[5].Filled);     // June
        Assert.False(page.Months[0].Filled);
        Assert.Equal("Sep", page.Months[8].Name);
        Assert.Equal(DateTimeOffset.Now.Year == 2026 ? 1 : 0, page.Months.Count(m => m.Current));
    }

    [Fact]
    public async Task PressingAMonthNarrowsTheYearToItAndAgainWidensIt()
    {
        var (page, _) = Create();
        await page.RefreshAsync();

        page.ToggleMonth(9);
        Assert.Equal(9, page.MonthFilter);
        Assert.Equal(["a", "b"], page.Cards.Select(c => c.Id));
        Assert.True(page.Months[8].Selected);
        Assert.Equal("3 reflections", page.CountLabel);  // the year's count, not the month's

        page.ToggleMonth(6);
        Assert.Equal(["c"], page.Cards.Select(c => c.Id));
        page.ToggleMonth(6);
        Assert.Equal(0, page.MonthFilter);
        Assert.Equal(3, page.Cards.Count);

        page.ToggleMonth(1);  // empty: nothing changes
        Assert.Equal(0, page.MonthFilter);

        page.ToggleMonth(9);
        page.ShowWholeYearCommand.Execute(null);  // the year itself widens
        Assert.Equal(0, page.MonthFilter);
        Assert.Equal(3, page.Cards.Count);

        page.ToggleMonth(9);
        page.Search = "smok";
        Assert.Equal(["b"], page.Cards.Select(c => c.Id));  // search within the month
        page.Search = "";
        await page.PreviousYearCommand.ExecuteAsync(null);
        Assert.Equal(0, page.MonthFilter);  // a new year starts wide
    }

    [Fact]
    public async Task EarlierYearsAreAStepAwayAndUntitledEntriesShowTheirDay()
    {
        var (page, _) = Create();
        await page.RefreshAsync();

        Assert.True(page.PreviousYearCommand.CanExecute(null));
        Assert.False(page.NextYearCommand.CanExecute(null));
        await page.PreviousYearCommand.ExecuteAsync(null);

        Assert.Equal(2025, page.Year);
        var card = Assert.Single(page.Cards);
        Assert.Equal("3 November", card.Title);
        Assert.True(page.NextYearCommand.CanExecute(null));
    }

    [Fact]
    public async Task SearchFiltersTitleAndWords()
    {
        var (page, _) = Create();
        await page.RefreshAsync();

        page.Search = "smok";
        Assert.Equal(["b"], page.Cards.Select(c => c.Id));
        page.Search = "back";
        Assert.Equal(["c"], page.Cards.Select(c => c.Id));
        page.Search = "sixties";
        Assert.Equal(["c"], page.Cards.Select(c => c.Id));  // the case study is searched too
        page.Search = "patient";
        Assert.Equal(["a", "c"], page.Cards.Select(c => c.Id));
        page.Search = "";
        Assert.Equal(3, page.Cards.Count);
    }

    [Fact]
    public async Task SearchReachesEveryAnswerOnceACardHasBeenOpened()
    {
        var (page, engine) = Create();
        engine.ReflectionAnswers = ("the line kept dropping", "l", "book a video review");
        engine.ReflectionSummary = "A patient in their forties.";
        await page.RefreshAsync();

        await page.ToggleAsync(page.Cards[0]);
        await page.ToggleAsync(page.Cards[0]);
        page.Search = "video review";
        Assert.Equal(["a"], page.Cards.Select(c => c.Id));
        page.Search = "dropping";
        Assert.Equal(["a"], page.Cards.Select(c => c.Id));
    }

    [Fact]
    public async Task OpeningACardLoadsItsEditorAndClosingSavesAndRefreshesTheLine()
    {
        var (page, engine) = Create();
        engine.ReflectionAnswers = ("h", "check the temperature", "n");
        engine.ReflectionSummary = "A patient in their forties.";
        await page.RefreshAsync();
        var card = page.Cards[0];

        await page.ToggleAsync(card);
        Assert.True(card.Expanded);
        Assert.NotNull(card.Editor);
        Assert.Equal("check the temperature", card.Editor!.Learned);

        card.Editor.Learned = "check the temperature and the pulse";
        card.Editor.Summary = "A patient in their forties with a hot elbow.";
        await page.ToggleAsync(page.Cards[1]);

        Assert.False(card.Expanded);
        Assert.Null(card.Editor);
        Assert.Equal("check the temperature and the pulse", card.Learned);
        Assert.Equal("A patient in their forties with a hot elbow.", card.Line);
        Assert.Contains(engine.Requests, r => r.Method == "reflection/update" && r.Params.Contains("pulse"));
        Assert.True(page.Cards[1].Expanded);
    }

    [Fact]
    public async Task ARetitleRenamesTheConsultationAndAnEmptySheetKeepsItsCard()
    {
        var (page, engine) = Create();
        await page.RefreshAsync();
        var card = page.Cards[2];  // Back pain: summary only, nothing written

        await page.ToggleAsync(card);
        card.Editor!.Title = "Back pain, missed red flags";
        await page.ToggleAsync(card);

        Assert.Contains(engine.Requests,
            r => r.Method == "session/label" && r.Params.Contains("missed red flags"));
        Assert.Equal("Back pain, missed red flags", card.Title);
        Assert.Equal(3, page.Cards.Count);
        Assert.Contains(card, page.Cards);
    }

    [Fact]
    public async Task RemovingACardTellsTheEngineAndDropsIt()
    {
        var (page, engine) = Create();
        await page.RefreshAsync();

        await page.DeleteAsync(page.Cards[0]);

        Assert.Contains(engine.Requests, r => r.Method == "reflection/delete" && r.Params.Contains("\"a\""));
        Assert.Equal(["b", "c"], page.Cards.Select(c => c.Id));
        Assert.Equal("2 reflections", page.CountLabel);
    }

    [Fact]
    public async Task NothingWrittenYetIsSaidPlainly()
    {
        var engine = new FakeEngineClient();
        var page = new AppraisalsViewModel(engine, new InlineDispatcher(), new StatusBarViewModel());

        await page.RefreshAsync();

        Assert.True(page.Empty);
        Assert.Empty(page.Cards);
        Assert.Equal(DateTimeOffset.Now.Year, page.Year);
    }
}
