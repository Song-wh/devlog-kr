using System.IO;
using System.Text.Json;

namespace CpuBench.App;

/// <summary>
/// 측정 결과를 이 PC에만 저장한다: %LOCALAPPDATA%\cpu-bench\results\yyyyMMdd-HHmmss-종류.json
/// 러너가 낸 JSON을 그대로 저장하므로 나중에 형식이 바뀌어도 원본이 남는다. 어디에도 보내지 않는다.
/// </summary>
public sealed class ResultStore
{
    public string Folder { get; }

    public ResultStore(string? folder = null)
    {
        Folder = folder ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "cpu-bench", "results");
    }

    public string Save(string kind, JsonElement json, DateTime? when = null)
    {
        Directory.CreateDirectory(Folder);
        var t = when ?? DateTime.Now;
        var path = Path.Combine(Folder, $"{t:yyyyMMdd-HHmmss}-{kind}.json");
        File.WriteAllText(path, json.GetRawText());
        return path;
    }

    /// <summary>가장 최근에 저장한 kind 결과(없거나 읽지 못하면 null). 앱을 다시 켜도 요약을 보여 주려고 쓴다.</summary>
    public JsonElement? LoadLatest(string kind)
    {
        if (!Directory.Exists(Folder)) return null;
        foreach (var f in Directory.GetFiles(Folder, $"*-{kind}.json").OrderByDescending(f => f).Take(3))
        {
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(f));
                return doc.RootElement.Clone();
            }
            catch (Exception e) when (e is IOException or JsonException) { }
        }
        return null;
    }

    public sealed record ScoreEntry(DateTime When, int Version, double Single, double Multi, bool Valid);

    /// <summary>저장된 점수 기록, 최신순. 읽을 수 없는 파일은 건너뛴다.</summary>
    public List<ScoreEntry> LoadScores(int max = 20)
    {
        if (!Directory.Exists(Folder)) return new();
        var list = new List<ScoreEntry>();
        foreach (var f in Directory.GetFiles(Folder, "*-score.json").OrderByDescending(f => f).Take(max))
        {
            try
            {
                var name = Path.GetFileName(f);
                var when = DateTime.ParseExact(name[..15], "yyyyMMdd-HHmmss", null);
                using var doc = JsonDocument.Parse(File.ReadAllText(f));
                var r = doc.RootElement;
                list.Add(new ScoreEntry(when, r.GetProperty("score_version").GetInt32(), r.GetProperty("single").GetDouble(),
                                        r.GetProperty("multi").GetDouble(), r.GetProperty("valid").GetBoolean()));
            }
            catch (Exception e) when (e is IOException or JsonException or FormatException or KeyNotFoundException or InvalidOperationException) { }
        }
        return list;
    }
}
