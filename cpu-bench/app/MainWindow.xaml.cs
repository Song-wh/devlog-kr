using System.Diagnostics;
using System.Text.Json;
using System.Windows;
using System.Windows.Documents;
using System.Windows.Media;

namespace CpuBench.App;

public partial class MainWindow : Window
{
    private readonly Runner? _runner = Runner.FindNextToApp();
    private readonly List<FindingRow> _findings = new();
    private readonly ResultStore _store = new();
    private readonly AppSettings _settings = AppSettings.Load();
    private string? _updateUrl;

    // 점수 항목 키 → 화면 이름
    private static readonly Dictionary<string, string> ItemNames = new()
    {
        ["int_st"] = "정수 (싱글)",
        ["mem_bw_st"] = "메모리 읽기 (싱글)",
        ["mem_lat_st"] = "메모리 지연 (싱글)",
        ["int_mt"] = "정수 (멀티)",
        ["mem_bw_mt"] = "메모리 읽기 (멀티)",
    };

    public MainWindow()
    {
        InitializeComponent();
        VersionRun.Text = $"  v{UpdateCheck.Current}";
        UpdateCheckBox.IsChecked = _settings.CheckUpdates;
        Loaded += async (_, _) =>
        {
            ShowHistory();
            await LoadInfoAsync();
            if (_settings.CheckUpdates) await CheckUpdateAsync();
        };
    }

    private async Task CheckUpdateAsync()
    {
        var r = await UpdateCheck.CheckAsync(UpdateCheck.Current, TimeSpan.FromSeconds(5));
        if (r is null) return;
        _updateUrl = r.Url;
        UpdateText.Text = $"새 버전 v{r.Version}이(가) 나왔습니다. 지금 버전은 v{UpdateCheck.Current}입니다.";
        UpdateBanner.Visibility = Visibility.Visible;
    }

    private void Update_Click(object sender, RoutedEventArgs e)
    {
        if (_updateUrl != null) Process.Start(new ProcessStartInfo(_updateUrl) { UseShellExecute = true });
    }

    private void UpdateCheckBox_Changed(object sender, RoutedEventArgs e)
    {
        bool on = UpdateCheckBox.IsChecked == true;
        if (on == _settings.CheckUpdates) return;
        _settings.CheckUpdates = on;
        _settings.Save();
        if (!on) UpdateBanner.Visibility = Visibility.Collapsed;
    }

    private async Task LoadInfoAsync()
    {
        if (_runner is null)
        {
            CpuText.Text = "cpu-bench-runner.exe를 찾지 못했습니다. 앱과 같은 폴더에 있어야 합니다.";
            CheckButton.IsEnabled = ScoreButton.IsEnabled = ThrottleButton.IsEnabled = false;
            return;
        }
        var r = await _runner.RunAsync("info --json");
        if (r.Json is null)
        {
            CpuText.Text = "PC 정보를 읽지 못했습니다.";
            return;
        }
        var j = r.Json.RootElement;
        CpuText.Text = $"{j.GetProperty("cpu").GetString()}  ·  논리 코어 {j.GetProperty("logical").GetInt32()}개" +
                       (j.GetProperty("hybrid").GetBoolean() ? $" (성능 코어 {j.GetProperty("perf_cores").GetInt32()}개 포함)" : "");
        if (j.TryGetProperty("memory", out var mem))
        {
            var mods = mem.GetProperty("modules").EnumerateArray().Where(m => m.GetProperty("size_bytes").GetInt64() > 0).ToList();
            int ch = mem.GetProperty("channels").GetInt32();
            string chText = ch switch { 1 => "싱글 채널", 2 => "듀얼 채널", 0 => "채널 구성 알 수 없음", _ => $"{ch}채널" };
            string speed = mods.Count > 0 ? $"{mods[0].GetProperty("type").GetString()} {mods[0].GetProperty("configured_speed").GetInt32()} MT/s" : "";
            MemText.Text = $"메모리 {mem.GetProperty("total_bytes").GetInt64() / (1L << 30)} GB · {mods.Count}개 모듈 · {chText} · {speed}";
        }
    }

    private void SetBusy(bool busy, string status)
    {
        CheckButton.IsEnabled = ScoreButton.IsEnabled = ThrottleButton.IsEnabled = !busy;
        Progress.Visibility = busy ? Visibility.Visible : Visibility.Hidden;
        Progress.IsIndeterminate = busy;
        StatusText.Text = status;
    }

    private void ShowFindings(IEnumerable<Finding> add, Func<Finding, bool>? replaceWhere = null)
    {
        if (replaceWhere != null) _findings.RemoveAll(r => replaceWhere(r.Source));
        _findings.AddRange(add.Select(f => new FindingRow(f)));
        Findings.ItemsSource = null;
        Findings.ItemsSource = _findings.OrderBy(f => f.Source.Ok).ToList();
        DiagHint.Visibility = _findings.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
    }

    private async void CheckButton_Click(object sender, RoutedEventArgs e)
    {
        if (_runner is null) return;
        SetBusy(true, "환경 점검 중… (3초 동안 쉬고 있을 때의 CPU 사용률을 잽니다)");
        // 점검은 '쉬고 있을 때' 사용률을 재므로, 이 앱도 그동안 아무것도 그리지 않는다.
        // 진행 막대 애니메이션을 켜 두면 앱 자신이 백그라운드 사용률 목록에 올라왔다(버그 005).
        Progress.Visibility = Visibility.Hidden;
        Progress.IsIndeterminate = false;
        var r = await _runner.RunAsync("check --json");
        SetBusy(false, r.Json is null ? $"환경 점검 실패: {r.StdErr.Trim()}" : "환경 점검 완료");
        if (r.Json is null) return;
        _store.Save("check", r.Json.RootElement);
        ShowFindings(Diagnosis.FromCheck(r.Json.RootElement), f => !f.Title.Contains("쓰로틀링"));
    }

    private async void ScoreButton_Click(object sender, RoutedEventArgs e)
    {
        if (_runner is null) return;
        SetBusy(true, "점수 측정 중…");
        Progress.IsIndeterminate = false;
        Progress.Value = 0;
        var r = await _runner.RunAsync("score --json --progress", p => Dispatcher.Invoke(() =>
        {
            int k = p.GetProperty("progress").GetInt32(), n = p.GetProperty("total").GetInt32();
            string key = p.GetProperty("item").GetString() ?? "";
            Progress.Value = (double)k / n;
            StatusText.Text = $"점수 측정 중… {k + 1}/{n} {ItemNames.GetValueOrDefault(key, key)}";
        }));
        if (r.Json is null)
        {
            SetBusy(false, $"점수 측정 실패: {r.StdErr.Trim()}");
            return;
        }
        var j = r.Json.RootElement;
        _store.Save("score", j);
        ShowHistory();
        bool valid = j.GetProperty("valid").GetBoolean();
        SingleText.Text = valid ? j.GetProperty("single").GetDouble().ToString("0") : "무효";
        MultiText.Text = valid ? j.GetProperty("multi").GetDouble().ToString("0") : "무효";
        ScoreItems.ItemsSource = j.GetProperty("items").EnumerateArray().Select(it =>
        {
            string key = it.GetProperty("key").GetString() ?? "";
            string unit = it.GetProperty("unit").GetString() ?? "";
            return new
            {
                Label = ItemNames.GetValueOrDefault(key, key),
                Value = $"{it.GetProperty("value").GetDouble():0.##} {unit}  (기준 {it.GetProperty("reference").GetDouble():0.##}, ×{it.GetProperty("ratio").GetDouble():0.00})",
            };
        }).ToList();
        int retries = j.GetProperty("items").EnumerateArray().Sum(it => it.TryGetProperty("retries", out var rr) ? rr.GetInt32() : 0);
        bool disturbed = j.TryGetProperty("disturbed", out var d) && d.GetBoolean();
        string note = disturbed ? " · 측정 중 다른 프로그램이 CPU를 많이 써서 결과가 낮게 나왔을 수 있습니다. 다른 프로그램을 닫고 다시 재 보세요."
                    : retries > 0 ? $" · 다른 프로그램 때문에 {retries}회를 다시 쟀습니다." : "";
        SetBusy(false, valid ? $"점수 측정 완료 (점수 체계 v{j.GetProperty("score_version").GetInt32()}){note}"
                             : "계산 결과가 기대값과 달라서 점수를 내지 않았습니다. 오버클럭이나 메모리 설정이 불안정할 수 있습니다.");
    }

    private async void ThrottleButton_Click(object sender, RoutedEventArgs e)
    {
        if (_runner is null) return;
        SetBusy(true, "쓰로틀링 검사 중… (1분 동안 모든 코어에 부하를 겁니다)");
        var r = await _runner.RunAsync("throttle --seconds 60 --json");
        SetBusy(false, r.Json is null ? $"쓰로틀링 검사 실패: {r.StdErr.Trim()}" : "쓰로틀링 검사 완료");
        if (r.Json is null) return;
        _store.Save("throttle", r.Json.RootElement);
        ShowFindings(new[] { Diagnosis.FromThrottle(r.Json.RootElement) }, f => f.Title.Contains("쓰로틀링"));
    }

    private void ShowHistory()
    {
        var list = _store.LoadScores(10);
        History.ItemsSource = list.Select(h => new
        {
            When = $"{h.When:yyyy-MM-dd HH:mm}  (v{h.Version})",
            Single = h.Valid ? $"싱글 {h.Single:0}" : "무효",
            Multi = h.Valid ? $"멀티 {h.Multi:0}" : "",
        }).ToList();
        HistoryHint.Visibility = list.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
    }

    private void OpenResults_Click(object sender, RoutedEventArgs e)
    {
        System.IO.Directory.CreateDirectory(_store.Folder);
        Process.Start(new ProcessStartInfo(_store.Folder) { UseShellExecute = true });
    }

    private void Settings_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Hyperlink { Tag: string uri } && !string.IsNullOrEmpty(uri))
            Process.Start(new ProcessStartInfo(uri) { UseShellExecute = true });
    }

    private sealed class FindingRow
    {
        public FindingRow(Finding f) => Source = f;
        public Finding Source { get; }
        public string Icon => Source.Ok ? "✔" : "⚠";
        public Brush IconBrush => (Brush)Application.Current.Resources[Source.Ok ? "Good" : "Accent"];
        public string Title => Source.Title;
        public string Detail => Source.Detail;
        public string? SettingsUri => Source.SettingsUri;
        public Visibility LinkVisibility => Source.SettingsUri is null ? Visibility.Collapsed : Visibility.Visible;
    }
}
