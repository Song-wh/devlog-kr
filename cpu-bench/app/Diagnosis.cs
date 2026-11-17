using System.Text.Json;
using System.Text.RegularExpressions;

namespace CpuBench.App;

/// <summary>진단 한 줄: 화면에 보여 줄 한국어 제목·설명과, 관련 Windows 설정 화면(있으면).</summary>
public sealed record Finding(bool Ok, string Title, string Detail, string? SettingsUri = null);

/// <summary>러너 JSON(경고 id·메시지)을 한국어 진단으로 바꾼다. 러너 출력은 영어라서 여기서 옮긴다.</summary>
public static partial class Diagnosis
{
    public static List<Finding> FromCheck(JsonElement check)
    {
        var list = new List<Finding>();
        double busy = check.GetProperty("background").GetProperty("busy_percent").GetDouble();
        var top = check.GetProperty("background").GetProperty("top").EnumerateArray()
            .Take(3).Select(p => $"{p.GetProperty("name").GetString()} {p.GetProperty("cpu_percent").GetDouble():0.0}%");
        var warnings = check.GetProperty("warnings").EnumerateArray()
            .Select(w => (Id: w.GetProperty("id").GetString() ?? "", Msg: w.GetProperty("message").GetString() ?? "")).ToList();

        foreach (var (id, msg) in warnings)
            list.Add(Translate(id, msg, busy, string.Join(", ", top)));

        // 경고가 없는 항목은 "정상"으로 보여 준다
        var ids = warnings.Select(w => w.Id).ToHashSet();
        if (!ids.Contains("on_battery") && !ids.Contains("power_saver") && !ids.Contains("efficiency_mode") && !ids.Contains("max_state"))
            list.Add(new Finding(true, "전원 설정 정상", "전원 연결, 전원 관리 옵션, 최대 프로세서 상태 모두 측정에 문제없습니다."));
        if (!ids.Contains("background"))
            list.Add(new Finding(true, $"백그라운드 사용률 {busy:0.0}%", "측정에 영향이 거의 없습니다."));
        if (!ids.Contains("single_channel") && !ids.Contains("same_channel") && !ids.Contains("xmp_off") && !ids.Contains("below_jedec"))
            list.Add(new Finding(true, "메모리 구성 정상", "채널 구성과 동작 속도에 문제가 보이지 않습니다."));
        return list.OrderBy(f => f.Ok).ToList();
    }

    public static Finding Translate(string id, string message, double busyPercent = 0, string topProcesses = "")
    {
        switch (id)
        {
            case "on_battery":
                return new(false, "배터리로 동작 중", "충전기를 연결하세요. 노트북은 배터리에서 클럭을 낮춥니다.", "ms-settings:batterysaver");
            case "power_saver":
                return new(false, "전원 관리 옵션이 '절전'", "균형 조정이나 고성능으로 바꾸세요.", "ms-settings:powersleep");
            case "efficiency_mode":
                return new(false, "전원 모드가 '최고의 전원 효율'", "'균형' 또는 '최고 성능'으로 바꾸세요.", "ms-settings:powersleep");
            case "max_state":
                return new(false, "최대 프로세서 상태가 100% 미만", "보통 터보 부스트가 꺼집니다. 전원 옵션 고급 설정에서 100%로 바꾸세요.", "ms-settings:powersleep");
            case "background":
                return new(false, $"쉬고 있을 때도 CPU를 {busyPercent:0.0}% 사용 중",
                    $"멀티스레드 점수가 그만큼 떨어질 수 있습니다. 사용률이 높은 프로그램: {topProcesses}", "taskmgr");
            case "single_channel":
                return new(false, "램 싱글 채널", "모듈이 1개라서 메모리 대역폭이 듀얼 채널의 절반쯤입니다.");
            case "same_channel":
                return new(false, "램이 같은 채널 슬롯에 몰려 있음", "메인보드 설명서의 권장 슬롯(보통 A2, B2)으로 옮기면 듀얼 채널이 됩니다.");
            case "xmp_off":
            {
                var m = XmpRegex().Match(message);
                return m.Success
                    ? new(false, "램이 정격보다 느리게 동작 (XMP/EXPO 꺼짐 의심)",
                        $"{m.Groups[1].Value} ({m.Groups[2].Value}): 정격 {m.Groups[3].Value} MT/s, 현재 {m.Groups[4].Value} MT/s. BIOS에서 XMP/EXPO 설정을 확인해 보세요.")
                    : new(false, "램이 정격보다 느리게 동작 (XMP/EXPO 꺼짐 의심)", message);
            }
            case "below_jedec":
            {
                var m = BelowRegex().Match(message);
                return m.Success
                    ? new(false, "램이 자체 기본 속도보다 느리게 동작",
                        $"{m.Groups[1].Value}: 현재 {m.Groups[2].Value} MT/s, 모듈 기본 {m.Groups[3].Value} MT/s. 서로 다른 램을 섞었거나 플랫폼 제한일 수 있습니다.")
                    : new(false, "램이 자체 기본 속도보다 느리게 동작", message);
            }
            default:
                return new(false, id, message);
        }
    }

    [GeneratedRegex(@"^(.+?) \((.+?)\) is rated (\d+) MT/s but runs at (\d+) MT/s")]
    private static partial Regex XmpRegex();

    [GeneratedRegex(@"^(.+?) runs at (\d+) MT/s, below its own (\d+) MT/s")]
    private static partial Regex BelowRegex();

    public static Finding FromThrottle(JsonElement t)
    {
        double tp = t.GetProperty("throughput_change").GetDouble();
        double p = t.GetProperty("p_change").GetDouble();
        double e = t.GetProperty("e_change").GetDouble();
        int eDrop = t.GetProperty("e_drop_at").GetInt32();
        bool hybrid = t.GetProperty("hybrid").GetBoolean();
        string clocks = hybrid ? $"P코어 클럭 {p:+0.0;-0.0}%, E코어 클럭 {e:+0.0;-0.0}%" : $"클럭 {p:+0.0;-0.0}%";
        string note = eDrop >= 0 ? $" E코어는 {eDrop}초째부터 내려갔습니다(전원 제한 또는 온도, 드라이버 없이는 구분 불가)." : "";
        return t.GetProperty("throttled").GetBoolean()
            ? new(false, "지속 부하에서 성능이 떨어짐 (쓰로틀링)", $"1분 동안 처리량 {tp:+0.0;-0.0}%, {clocks}. 쿨러와 BIOS 전원 제한을 확인해 보세요.{note}")
            : new(true, "쓰로틀링 없음", $"1분 동안 처리량 {tp:+0.0;-0.0}%, {clocks}.{note}");
    }
}
