using System.IO;
using System.Text.Json;

namespace CpuBench.App;

/// <summary>앱 설정: %LOCALAPPDATA%\cpu-bench\settings.json. 읽지 못하면 기본값.</summary>
public sealed class AppSettings
{
    /// <summary>앱을 켤 때 새 버전이 있는지 GitHub에서 확인한다 (기본 켬)</summary>
    public bool CheckUpdates { get; set; } = true;

    /// <summary>인터넷 속도 측정(M-Lab, 결과와 IP가 공개 데이터로 남음)에 동의했는지. 처음 누를 때 묻는다.</summary>
    public bool MlabConsent { get; set; }

    private static string FilePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "cpu-bench", "settings.json");

    public static AppSettings Load()
    {
        try
        {
            return JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath)) ?? new AppSettings();
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
            return new AppSettings();
        }
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
}
