using System.Text.Json;
using Ambient.App.Core.Metrics;

namespace Ambient.App.Tests;

public class PerformanceCollectorTest : IDisposable
{
    private readonly string _path = Path.Combine(
        Path.GetTempPath(), Path.GetRandomFileName(), "metrics.jsonl");

    public void Dispose()
    {
        try
        {
            Directory.Delete(Path.GetDirectoryName(_path)!, recursive: true);
        }
        catch (IOException)
        {
        }

        GC.SuppressFinalize(this);
    }

    private PerformanceCollector NewCollector(FakeEngineClient engine, bool enabled = true) =>
        new(engine, () => enabled, () => null, _path);

    [Fact]
    public async Task AFinishedSessionAppendsOneLine()
    {
        var engine = new FakeEngineClient();
        var collector = NewCollector(engine);

        collector.NoteModel("Qwen3.5 9B", "default", 24.1);
        collector.SessionStarted("replay", 1.0, "Elbow swelling");
        collector.StopRequested();
        collector.NotePartial(15.3);
        collector.NotePartial(16.1);
        collector.NoteReady(17.4);
        collector.PatientPartial(14.0);
        await collector.SessionFinishedAsync(null, 1290, patientTokensPerSecond: 16.0);

        var line = Assert.Single(File.ReadAllLines(_path));
        using var record = JsonDocument.Parse(line);
        var root = record.RootElement;
        Assert.Equal("replay", root.GetProperty("source").GetString());
        Assert.Equal(1.0, root.GetProperty("replaySpeed").GetDouble());
        Assert.Equal("Elbow swelling", root.GetProperty("track").GetString());
        Assert.Equal(33.4, root.GetProperty("engine").GetProperty("asrRealtimeFactor").GetDouble());
        Assert.Equal(1290, root.GetProperty("note").GetProperty("chars").GetInt32());
        Assert.True(root.GetProperty("note").GetProperty("firstPartialAfterStopSeconds")
            .GetDouble() >= 0);
        Assert.True(root.GetProperty("memory").GetProperty("availableAtStartMb").GetInt64() > 0);
        Assert.Equal(2, root.GetProperty("schema").GetInt32());
        Assert.Equal("completed", root.GetProperty("outcome").GetString());
        var note = root.GetProperty("note");
        Assert.Equal("Qwen3.5 9B", note.GetProperty("model").GetString());
        Assert.Equal("default", note.GetProperty("tier").GetString());
        Assert.Equal(24.1, note.GetProperty("modelLoadSeconds").GetDouble());
        Assert.Equal(17.4, note.GetProperty("tokensPerSecond").GetDouble());
        Assert.True(note.GetProperty("readyAfterStopSeconds").GetDouble() >= 0);
        var patient = root.GetProperty("patient");
        Assert.True(patient.GetProperty("readyAfterNoteSeconds").GetDouble() >= 0);
        Assert.Equal(16.0, patient.GetProperty("tokensPerSecond").GetDouble());
    }

    [Fact]
    public async Task DisabledCollectionWritesNothing()
    {
        var collector = NewCollector(new FakeEngineClient(), enabled: false);

        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        await collector.SessionFinishedAsync(null, 100);

        Assert.False(File.Exists(_path));
    }

    [Fact]
    public async Task AFailedNoteIsRecordedWithItsReason()
    {
        var engine = new FakeEngineClient();
        var collector = NewCollector(engine);

        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        await collector.SessionFinishedAsync("the transcript is empty", 0);
        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        await collector.SessionFinishedAsync(null, 42);

        var lines = File.ReadAllLines(_path);
        Assert.Equal(2, lines.Length);
        using var failed = JsonDocument.Parse(lines[0]);
        Assert.Equal("clinical note failed", failed.RootElement.GetProperty("outcome").GetString());
        Assert.False(failed.RootElement.TryGetProperty("patient", out _));
        Assert.Equal("the transcript is empty",
            failed.RootElement.GetProperty("note").GetProperty("failed").GetString());
        using var fine = JsonDocument.Parse(lines[1]);
        Assert.False(fine.RootElement.GetProperty("note").TryGetProperty("failed", out _));
    }

    [Fact]
    public async Task RefusalsAndPatientNoteFailuresNameTheirOutcome()
    {
        var collector = NewCollector(new FakeEngineClient());

        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        await collector.SessionFinishedAsync("refused: not a consultation", 0);
        collector.SessionStarted("mic", 0, null);
        collector.StopRequested();
        collector.NotePartial();
        collector.NoteReady();
        await collector.SessionFinishedAsync(null, 300, "failed");

        var lines = File.ReadAllLines(_path);
        using var refused = JsonDocument.Parse(lines[0]);
        Assert.Equal("refused", refused.RootElement.GetProperty("outcome").GetString());
        using var patientFailed = JsonDocument.Parse(lines[1]);
        Assert.Equal("patient note failed", patientFailed.RootElement.GetProperty("outcome").GetString());
        var patient = patientFailed.RootElement.GetProperty("patient");
        Assert.Equal("failed", patient.GetProperty("failed").GetString());
        Assert.False(patient.TryGetProperty("readyAfterNoteSeconds", out _));
    }

    [Fact]
    public async Task AFinishWithoutAStopIsIgnored()
    {
        var collector = NewCollector(new FakeEngineClient());
        await collector.SessionFinishedAsync(null, 5);
        Assert.False(File.Exists(_path));
    }
}
