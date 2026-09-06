using System.Text;

namespace Ambient.App.Core.Appraisal;

/// <summary>One appraisal reflection as it leaves the device: no identifiers, month precision.</summary>
public sealed record ReflectionEntry(
    string Title, string Month, string Summary, string Happened, string Learned, string Next)
{
    public bool IsEmpty =>
        Happened.Trim().Length == 0 && Learned.Trim().Length == 0 && Next.Trim().Length == 0;
}

/// <summary>
/// Plain text for appraisal portfolios, which all take free text.
/// </summary>
public static class ReflectionExport
{
    public const string Declaration =
        "The case study was prepared by Ambient from the clinical note and anonymised. "
        + "The reflection is the clinician's own words.";

    public static string Format(ReflectionEntry entry)
    {
        var text = new StringBuilder();
        text.Append(entry.Title.Length > 0 ? entry.Title : "Consultation").Append('\n');
        text.Append(entry.Month).Append("\n\n");
        var hasSummary = entry.Summary.Trim().Length > 0;
        if (hasSummary)
        {
            text.Append("Case study\n").Append(entry.Summary.Trim()).Append("\n\n");
        }

        Section(text, "What stood out?", entry.Happened);
        Section(text, "What did I learn?", entry.Learned);
        Section(text, "Would I do anything differently?", entry.Next);
        if (hasSummary)
        {
            text.Append(Declaration).Append('\n');
        }

        return text.ToString().TrimEnd() + "\n";
    }

    private static void Section(StringBuilder text, string question, string answer)
    {
        if (answer.Trim().Length == 0)
        {
            return;
        }

        text.Append(question).Append('\n').Append(answer.Trim()).Append("\n\n");
    }
}
