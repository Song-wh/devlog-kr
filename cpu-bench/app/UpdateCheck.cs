using System.Net.Http;
using System.Text.Json;

namespace CpuBench.App;

/// <summary>
/// 새 버전 확인. GitHub Releases에서 태그가 "cpu-bench-v"로 시작하는 정식 릴리스 중 가장 높은 버전을 찾아
/// 지금 버전보다 높으면 알려 준다. 알림만 하고, 내려받기나 교체는 하지 않는다.
/// 보내는 것은 GitHub API 요청 하나뿐이다(측정 결과나 PC 정보는 보내지 않음).
/// </summary>
public static class UpdateCheck
{
    public const string TagPrefix = "cpu-bench-v";
    private const string ReleasesUrl = "https://api.github.com/repos/Song-wh/devlog-kr/releases?per_page=50";

    public sealed record Release(Version Version, string Tag, string Url);

    /// <summary>현재 앱 버전 (VERSION 파일 값, 세 자리)</summary>
    public static Version Current
    {
        get
        {
            var v = typeof(UpdateCheck).Assembly.GetName().Version ?? new Version(0, 0, 0);
            return new Version(v.Major, v.Minor, Math.Max(v.Build, 0));
        }
    }

    /// <summary>새 버전이 있으면 그 릴리스, 없거나 확인하지 못하면 null. 실패는 조용히 넘긴다.</summary>
    public static async Task<Release?> CheckAsync(Version current, TimeSpan timeout)
    {
        try
        {
            using var http = new HttpClient { Timeout = timeout };
            // GitHub API는 User-Agent가 필요하다. 프로그램 이름과 버전만 보낸다.
            http.DefaultRequestHeaders.UserAgent.ParseAdd($"cpu-bench/{current}");
            http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
            using var doc = JsonDocument.Parse(await http.GetStringAsync(ReleasesUrl));
            return FindNewer(doc.RootElement, current);
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or JsonException or InvalidOperationException)
        {
            return null;
        }
    }

    /// <summary>릴리스 목록(JSON 배열)에서 current보다 높은 가장 새 정식 릴리스. 초안·사전 릴리스·다른 프로그램 태그는 건너뛴다.</summary>
    public static Release? FindNewer(JsonElement releases, Version current)
    {
        if (releases.ValueKind != JsonValueKind.Array) return null;
        Release? best = null;
        foreach (var r in releases.EnumerateArray())
        {
            if (r.TryGetProperty("draft", out var d) && d.ValueKind == JsonValueKind.True) continue;
            if (r.TryGetProperty("prerelease", out var p) && p.ValueKind == JsonValueKind.True) continue;
            if (!r.TryGetProperty("tag_name", out var t) || t.GetString() is not string tag) continue;
            if (!tag.StartsWith(TagPrefix, StringComparison.Ordinal)) continue;
            if (!Version.TryParse(tag[TagPrefix.Length..], out var v)) continue;
            v = new Version(v.Major, v.Minor, Math.Max(v.Build, 0));
            if (v <= current || (best != null && v <= best.Version)) continue;
            string url = r.TryGetProperty("html_url", out var u) && u.GetString() is string s ? s : "https://github.com/Song-wh/devlog-kr/releases";
            best = new Release(v, tag, url);
        }
        return best;
    }
}
