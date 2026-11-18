using System.Net.Http;
using System.Net.WebSockets;
using System.Text.Json;

namespace CpuBench.App;

/// <summary>
/// 인터넷 속도 측정: M-Lab(Measurement Lab)의 공개 측정 서버와 ndt7 방식으로 다운로드·업로드를 10초씩 잰다.
/// M-Lab은 외부 프로그램이 쓰도록 만든 공개 연구 플랫폼이며, 측정 결과와 공인 IP 주소가 공개 데이터로 남는다.
/// 그래서 사용자가 버튼을 누르고 동의했을 때만 실행한다. PC 측정 결과는 보내지 않는다.
/// 참고: https://www.measurementlab.net/develop/ , ndt7 프로토콜 https://github.com/m-lab/ndt-server/blob/main/spec/ndt7-protocol.md
/// </summary>
public static class NetSpeed
{
    public sealed record Result(double DownloadMbps, double UploadMbps, double MinRttMs, string Server, string City);

    private const string Subprotocol = "net.measurementlab.ndt.v7";
    private static readonly TimeSpan Duration = TimeSpan.FromSeconds(10);

    public static async Task<Result> RunAsync(string clientVersion, IProgress<string> progress, CancellationToken ct)
    {
        progress.Report("가장 가까운 M-Lab 서버를 찾는 중…");
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd($"cpu-bench/{clientVersion}");
        string locate = $"https://locate.measurementlab.net/v2/nearest/ndt/ndt7?client_name=cpu-bench&client_version={clientVersion}";
        using var doc = JsonDocument.Parse(await http.GetStringAsync(locate, ct));
        var first = doc.RootElement.GetProperty("results")[0];
        string machine = first.GetProperty("machine").GetString() ?? "";
        string city = first.TryGetProperty("location", out var loc) && loc.TryGetProperty("city", out var c) ? c.GetString() ?? "" : "";
        var urls = first.GetProperty("urls");
        string down = urls.GetProperty("wss:///ndt/v7/download").GetString()!;
        string up = urls.GetProperty("wss:///ndt/v7/upload").GetString()!;

        progress.Report($"다운로드 측정 중… (M-Lab {city}, 10초)");
        var (dl, rtt) = await DownloadAsync(down, ct);
        progress.Report($"업로드 측정 중… (M-Lab {city}, 10초)");
        double ul = await UploadAsync(up, ct);
        return new Result(dl, ul, rtt, machine, city);
    }

    private static ClientWebSocket NewSocket()
    {
        var ws = new ClientWebSocket();
        ws.Options.AddSubProtocol(Subprotocol);
        return ws;
    }

    /// <summary>서버가 보내는 데이터를 받은 양으로 다운로드 속도를 잰다. 최소 RTT는 서버가 보내는 TCPInfo.MinRTT(마이크로초).</summary>
    private static async Task<(double Mbps, double MinRttMs)> DownloadAsync(string url, CancellationToken ct)
    {
        using var ws = NewSocket();
        await ws.ConnectAsync(new Uri(url), ct);
        var buf = new byte[1 << 20];
        long bytes = 0;
        double minRtt = 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        using var limit = CancellationTokenSource.CreateLinkedTokenSource(ct);
        limit.CancelAfter(Duration + TimeSpan.FromSeconds(5));
        try
        {
            while (ws.State == WebSocketState.Open)
            {
                var r = await ws.ReceiveAsync(buf, limit.Token);
                if (r.MessageType == WebSocketMessageType.Close) break;
                bytes += r.Count;
                if (r.MessageType == WebSocketMessageType.Text && r.EndOfMessage)
                {
                    try
                    {
                        using var m = JsonDocument.Parse(buf.AsMemory(0, r.Count));
                        if (m.RootElement.TryGetProperty("TCPInfo", out var ti) && ti.TryGetProperty("MinRTT", out var rtt))
                            minRtt = rtt.GetDouble() / 1000.0;
                    }
                    catch (JsonException) { }
                }
            }
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested) { }
        double sec = sw.Elapsed.TotalSeconds;
        return (sec > 0 ? bytes * 8 / sec / 1e6 : 0, minRtt);
    }

    /// <summary>
    /// 10초 동안 보낸다. 보낸 양은 운영체제 버퍼에 쌓인 것까지 포함돼 실제보다 클 수 있어서,
    /// 서버가 알려 주는 TCPInfo.BytesReceived / ElapsedTime이 있으면 그것을 쓴다.
    /// </summary>
    private static async Task<double> UploadAsync(string url, CancellationToken ct)
    {
        using var ws = NewSocket();
        await ws.ConnectAsync(new Uri(url), ct);
        var payload = new byte[1 << 13];  // 8 KiB부터 시작해 보낸 양의 1/16까지 키운다(ndt7 권장, 최대 1 MiB)
        Random.Shared.NextBytes(payload);
        long sent = 0;
        double serverMbps = 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();

        var reader = Task.Run(async () =>
        {
            var buf = new byte[1 << 16];
            try
            {
                while (ws.State == WebSocketState.Open)
                {
                    var r = await ws.ReceiveAsync(buf, ct);
                    if (r.MessageType == WebSocketMessageType.Close) break;
                    if (r.MessageType != WebSocketMessageType.Text || !r.EndOfMessage) continue;
                    try
                    {
                        using var m = JsonDocument.Parse(buf.AsMemory(0, r.Count));
                        if (m.RootElement.TryGetProperty("TCPInfo", out var ti) && ti.TryGetProperty("BytesReceived", out var br) &&
                            ti.TryGetProperty("ElapsedTime", out var el) && el.GetDouble() > 0)
                            serverMbps = br.GetDouble() * 8 / (el.GetDouble() / 1e6) / 1e6;
                    }
                    catch (JsonException) { }
                }
            }
            catch (Exception e) when (e is WebSocketException or OperationCanceledException) { }
        }, ct);

        while (sw.Elapsed < Duration && ws.State == WebSocketState.Open)
        {
            await ws.SendAsync(payload, WebSocketMessageType.Binary, true, ct);
            sent += payload.Length;
            if (payload.Length < (1 << 20) && payload.Length < sent / 16)
            {
                payload = new byte[payload.Length * 2];
                Random.Shared.NextBytes(payload);
            }
        }
        double sec = sw.Elapsed.TotalSeconds;
        try { await ws.CloseAsync(WebSocketCloseStatus.NormalClosure, "", ct); } catch (WebSocketException) { }
        await Task.WhenAny(reader, Task.Delay(2000, ct));
        return serverMbps > 0 ? serverMbps : (sec > 0 ? sent * 8 / sec / 1e6 : 0);
    }
}
