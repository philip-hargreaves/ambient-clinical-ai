using System.Text.Json;
using Ambient.App.Core.Appraisal;

namespace Ambient.App.Tests;

public class ReflectionExportTest
{
    [Fact]
    public void FormatCarriesTitleMonthCaseStudyAnsweredQuestionsAndTheDeclaration()
    {
        var text = ReflectionExport.Format(new ReflectionEntry(
            "Elbow swelling", "September 2026", "A patient in their forties.",
            "Reassured too quickly.", "", "Check for infection first."));

        Assert.StartsWith("Elbow swelling\nSeptember 2026\n\nCase study\nA patient in their forties.\n\n", text);
        Assert.Contains("What stood out?\nReassured too quickly.\n", text);
        Assert.DoesNotContain("What did I learn?", text);
        Assert.Contains("Would I do anything differently?\nCheck for infection first.\n", text);
        Assert.EndsWith("first.\n\n" + ReflectionExport.Declaration + "\n", text);
    }

    [Fact]
    public void WithoutACaseStudyThereIsNothingToDeclare()
    {
        var text = ReflectionExport.Format(new ReflectionEntry("", "May 2026", "", "x", "", ""));

        Assert.StartsWith("Consultation\nMay 2026\n\nWhat stood out?\nx\n", text);
        Assert.DoesNotContain("Case study", text);
        Assert.DoesNotContain(ReflectionExport.Declaration, text);
    }

    [Fact]
    public void AnEntryWithNoAnswersIsEmpty()
    {
        Assert.True(new ReflectionEntry("t", "m", "summary", " ", "", "\n").IsEmpty);
        Assert.False(new ReflectionEntry("t", "m", "", "", "learned", "").IsEmpty);
    }

    [Theory]
    [InlineData("Mrs Patel came in worried", "a name: Mrs Patel")]
    [InlineData("seen on 12/03/2026 at the surgery", "a date: 12/03/2026")]
    [InlineData("seen on 3 March", "a date: 3 March")]
    [InlineData("a 43-year-old man", "an exact age: 43-year-old")]
    [InlineData("the patient, aged 67,", "an exact age: aged 67")]
    [InlineData("NHS number 943 476 5919", "an NHS number: 943 476 5919")]
    [InlineData("lives at SW1A 1AA", "a postcode: SW1A 1AA")]
    [InlineData("call 020 7946 0958", "a phone number: 020 7946 0958")]
    public void TheCheckNamesWhatMayIdentifyThePatient(string text, string expected)
    {
        Assert.Contains(expected, IdentifierCheck.Find(text));
        Assert.StartsWith("This may identify the patient: ", IdentifierCheck.Describe(text));
    }

    [Fact]
    public void AnonymisedTextPassesClean()
    {
        const string text = "A patient in their forties presented with a week of painless swelling "
                            + "over one elbow. Olecranon bursitis was suspected; rest and an "
                            + "anti-inflammatory were agreed. I reassured too quickly.";
        Assert.Empty(IdentifierCheck.Find(text));
        Assert.Equal("", IdentifierCheck.Describe(text));
    }

    // Shared fixture: outputs must pass the check, scrubbed inputs must fail it
    [Fact]
    public void TheEngineScrubFixtureAgreesWithTheCheck()
    {
        var dir = AppContext.BaseDirectory;
        while (dir is not null && !Directory.Exists(Path.Combine(dir, "schema", "fixtures")))
        {
            dir = Path.GetDirectoryName(dir);
        }

        Assert.NotNull(dir);
        var rows = JsonDocument.Parse(
            File.ReadAllText(Path.Combine(dir, "schema", "fixtures", "summary-scrub.json"))).RootElement;

        foreach (var row in rows.EnumerateArray())
        {
            var input = row.GetProperty("in").GetString()!;
            var output = row.GetProperty("out").GetString()!;
            Assert.Empty(IdentifierCheck.Find(output));
            if (input != output)
            {
                Assert.NotEmpty(IdentifierCheck.Find(input));
            }
        }
    }
}
