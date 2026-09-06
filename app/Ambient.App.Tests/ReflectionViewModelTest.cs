using System.Text.Json;
using Ambient.App.Core.ViewModels;

namespace Ambient.App.Tests;

public class ReflectionViewModelTest
{
    private static (ReflectionViewModel Reflection, FakeEngineClient Engine) Create()
    {
        var engine = new FakeEngineClient();
        return (new ReflectionViewModel(engine, new InlineDispatcher(), new StatusBarViewModel()), engine);
    }

    [Fact]
    public async Task OpeningANewEntryAsksForTheSummaryAndStartsEmpty()
    {
        var (reflection, engine) = Create();

        await reflection.LoadAsync("abc", "2026-09-04T09:12:00Z");

        Assert.Equal("Elbow swelling", reflection.Title);
        Assert.Equal("September 2026", reflection.Month);
        Assert.Contains(engine.Requests, r => r.Method == "reflection/summary" && r.Params.Contains("abc"));
        Assert.Equal("A patient in their forties presented with a swollen elbow.", reflection.Summary);
        Assert.False(reflection.SummaryPending);
        Assert.Equal("", reflection.Happened);
        Assert.False(reflection.Dirty);
    }

    [Fact]
    public async Task AStoredEntryLoadsWithoutAskingForAnotherSummary()
    {
        var (reflection, engine) = Create();
        engine.ReflectionSummary = "A patient in their thirties with a cough.";
        engine.ReflectionAnswers = ("h", "l", "n");

        await reflection.LoadAsync("abc");

        Assert.DoesNotContain(engine.Requests, r => r.Method == "reflection/summary");
        Assert.Equal("A patient in their thirties with a cough.", reflection.Summary);
        Assert.Equal(("h", "l", "n"), (reflection.Happened, reflection.Learned, reflection.Next));
    }

    [Fact]
    public async Task SaveSendsOnlyWhenSomethingChanged()
    {
        var (reflection, engine) = Create();
        await reflection.LoadAsync("abc");

        await reflection.SaveAsync();
        Assert.DoesNotContain(engine.Requests, r => r.Method == "reflection/update");

        reflection.Learned = "check the temperature";
        Assert.True(reflection.Dirty);
        await reflection.SaveAsync();
        var update = Assert.Single(engine.Requests, r => r.Method == "reflection/update");
        var sent = JsonDocument.Parse(update.Params).RootElement;
        Assert.Equal("abc", sent.GetProperty("id").GetString());
        Assert.Equal("check the temperature", sent.GetProperty("learned").GetString());
        Assert.Equal("", sent.GetProperty("happened").GetString());
        Assert.False(reflection.Dirty);

        await reflection.SaveAsync();
        Assert.Single(engine.Requests, r => r.Method == "reflection/update");
    }

    [Fact]
    public async Task AStrayLineBreakIsNotAnAnswer()
    {
        var (reflection, engine) = Create();
        engine.ReflectionAnswers = ("\r\n", "l", " ");
        await reflection.LoadAsync("abc");
        Assert.Equal("", reflection.Happened);
        Assert.Equal("", reflection.Next);

        reflection.Happened = "\n";
        await reflection.SaveAsync();
        Assert.Equal("", reflection.Happened);
        Assert.DoesNotContain(engine.Requests, r => r.Method == "reflection/update");

        reflection.Happened = "  a real answer ";
        await reflection.SaveAsync();
        var update = Assert.Single(engine.Requests, r => r.Method == "reflection/update");
        Assert.Contains("a real answer", update.Params);
    }

    [Fact]
    public async Task AFailedSummarySaysSoAndRewriteTriesAgain()
    {
        var (reflection, engine) = Create();
        engine.SummaryFails = true;

        await reflection.LoadAsync("abc");

        Assert.False(reflection.HasSummary);
        Assert.Contains("the model is not loaded", reflection.SummaryProblem);
        Assert.False(reflection.SummaryPending);

        engine.SummaryFails = false;
        await reflection.RewriteSummaryCommand.ExecuteAsync(null);
        Assert.True(reflection.HasSummary);
        Assert.Equal("", reflection.SummaryProblem);
    }

    [Fact]
    public async Task ASummaryForAnotherSessionIsIgnored()
    {
        var (reflection, engine) = Create();
        engine.ReflectionSummary = "mine";
        await reflection.LoadAsync("abc");

        engine.RaiseNotification("reflection/summary",
            JsonSerializer.SerializeToElement(new { id = "other", text = "theirs" }));

        Assert.Equal("mine", reflection.Summary);
    }

    [Fact]
    public async Task TheWarningNamesAnIdentifierInAnyBox()
    {
        var (reflection, _) = Create();
        await reflection.LoadAsync("abc");

        Assert.False(reflection.HasWarning);
        reflection.Happened = "Mrs Patel was upset";
        Assert.Contains("Mrs Patel", reflection.Warning);
    }

    [Fact]
    public async Task ATitleSavesAsTheConsultationsLabelOnlyWhenChanged()
    {
        var (reflection, engine) = Create();
        await reflection.LoadAsync("abc");

        await reflection.SaveTitleAsync();
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/label");

        reflection.Title = "  ";
        await reflection.SaveTitleAsync();
        Assert.DoesNotContain(engine.Requests, r => r.Method == "session/label");
        Assert.Equal(reflection.Month, reflection.DisplayTitle);

        reflection.Title = "Elbow swelling, missed infection";
        await reflection.SaveTitleAsync();
        var rename = Assert.Single(engine.Requests, r => r.Method == "session/label");
        Assert.Contains("missed infection", rename.Params);
        await reflection.SaveTitleAsync();
        Assert.Single(engine.Requests, r => r.Method == "session/label");
    }

    [Fact]
    public async Task ExportTextIsThePlainEntry()
    {
        var (reflection, _) = Create();
        await reflection.LoadAsync("abc", "2026-09-04T09:12:00Z");
        reflection.Learned = "check the temperature";

        Assert.StartsWith("Elbow swelling\nSeptember 2026\n\nCase study\n", reflection.ExportText);
        Assert.Contains("What did I learn?\ncheck the temperature", reflection.ExportText);
    }
}
