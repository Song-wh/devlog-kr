using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;

namespace CpuBench.App;

/// <summary>
/// cpu-bench-runner.exe 실행기. 러너를 자식 프로세스로 띄우고 --json 출력(stdout)을 읽는다.
/// 진행률은 러너가 stderr에 한 줄씩 쓰는 JSON({"progress":k,"total":n,...})을 읽는다.
/// 측정은 별도 프로세스라서, 러너가 죽어도 앱은 살아 있다.
/// </summary>
public sealed class Runner
{
    public string Path { get; }

    public Runner(string path) => Path = path;

    /// <summary>앱 옆의 cpu-bench-runner.exe. 없으면 null.</summary>
    public static Runner? FindNextToApp()
    {
        var p = System.IO.Path.Combine(AppContext.BaseDirectory, "cpu-bench-runner.exe");
        return File.Exists(p) ? new Runner(p) : null;
    }

    public sealed record Result(int ExitCode, JsonDocument? Json, string StdErr);

    public async Task<Result> RunAsync(string args, Action<JsonElement>? onProgress = null, CancellationToken ct = default)
    {
        var psi = new ProcessStartInfo(Path, args)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        using var proc = new Process { StartInfo = psi };
        var err = new StringBuilder();
        proc.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is null) return;
            if (onProgress != null && e.Data.StartsWith("{\"progress\"", StringComparison.Ordinal))
            {
                try
                {
                    using var doc = JsonDocument.Parse(e.Data);
                    onProgress(doc.RootElement.Clone());
                    return;
                }
                catch (JsonException) { }
            }
            err.AppendLine(e.Data);
        };
        proc.Start();
        proc.BeginErrorReadLine();
        string stdout;
        using (ct.Register(() => { try { proc.Kill(entireProcessTree: true); } catch { } }))
        {
            stdout = await proc.StandardOutput.ReadToEndAsync(ct).ConfigureAwait(false);
            await proc.WaitForExitAsync(ct).ConfigureAwait(false);
        }
        JsonDocument? json = null;
        var line = stdout.Split('\n').LastOrDefault(l => l.TrimStart().StartsWith('{'));
        if (line != null)
        {
            try { json = JsonDocument.Parse(line); } catch (JsonException) { }
        }
        return new Result(proc.ExitCode, json, err.ToString());
    }
}
