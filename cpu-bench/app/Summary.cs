using System.Text.Json;

namespace CpuBench.App;

/// <summary>요약 카드 하나. State: ok(정상) / warn(확인 필요) / none(아직 안 잼)</summary>
public sealed record SummaryCard(string State, string Title, string Value, string Detail);

/// <summary>
/// 전문 용어 없이 "지금 내 PC가 어떤 상태인지"를 보여 주는 요약.
/// 다른 PC와 비교하는 판정은 하지 않는다(비교 데이터가 없음). 이 PC 안에서 잴 수 있는 "제 성능을 내는지"만 판정한다:
/// 발열로 성능이 떨어지는지, 메모리가 이론 속도를 내는지, 쉴 때 뭔가 CPU를 쓰는지, 전원 설정이 성능을 막는지.
/// </summary>
public static class Summary
{
    /// <summary>메모리 멀티 읽기가 이론 대역폭의 이 비율보다 낮으면 "확인 필요". 개발 PC는 91%(DDR5 듀얼). 가정값 — 데이터가 모이면 조정</summary>
    public const double MemEfficiencyWarn = 0.80;

    public static (string State, string Verdict, List<SummaryCard> Cards) Build(JsonElement? info, JsonElement? check, JsonElement? score, JsonElement? throttle)
    {
        var warnIds = check?.GetProperty("warnings").EnumerateArray().Select(w => w.GetProperty("id").GetString() ?? "").ToHashSet() ?? new();
        var cards = new List<SummaryCard> { Cooling(throttle), Memory(info, check, score, warnIds), Background(check, warnIds), Power(check, warnIds) };

        int warn = cards.Count(c => c.State == "warn");
        int none = cards.Count(c => c.State == "none");
        if (none == cards.Count)
            return ("none", "아직 점검하지 않았습니다. '전체 점검'을 누르면 2분 30초쯤 걸립니다.", cards);
        if (warn > 0)
            return ("warn", $"성능을 다 못 내게 할 수 있는 것이 {warn}가지 있습니다", cards);
        if (none > 0)
            return ("none", "지금까지 잰 항목은 모두 정상입니다. 나머지도 재려면 '전체 점검'을 누르세요.", cards);
        return ("ok", "이 PC는 제 성능을 내고 있습니다", cards);
    }

    private static SummaryCard Cooling(JsonElement? t)
    {
        const string title = "발열 · 성능 유지";
        if (t is not JsonElement th) return new("none", title, "아직 안 잼", "1분 동안 모든 코어에 부하를 걸어 성능이 유지되는지 봅니다.");
        double keep = 100 + th.GetProperty("throughput_change").GetDouble();
        return th.GetProperty("throttled").GetBoolean()
            ? new("warn", title, $"1분 뒤 성능 {keep:0}%", "오래 일을 시키면 느려집니다. 쿨러 상태나 BIOS 전원 제한을 확인해 보세요.")
            : new("ok", title, $"1분 내내 {Math.Min(keep, 100):0}% 유지", "오래 일을 시켜도 느려지지 않습니다.");
    }

    private static SummaryCard Memory(JsonElement? info, JsonElement? check, JsonElement? score, HashSet<string> warnIds)
    {
        const string title = "메모리";
        if (warnIds.Contains("single_channel"))
            return new("warn", title, "싱글 채널", "램이 한 장이라 속도가 듀얼 채널의 절반쯤입니다. 같은 램을 한 장 더 꽂으면 좋아집니다.");
        if (warnIds.Contains("same_channel"))
            return new("warn", title, "슬롯 위치 확인", "램이 같은 채널에 몰려 있습니다. 메인보드 설명서의 권장 슬롯(보통 A2, B2)으로 옮기세요.");
        if (warnIds.Contains("xmp_off"))
            return new("warn", title, "정격보다 느림", "램이 제 속도보다 느리게 동작합니다. BIOS에서 XMP/EXPO 설정을 확인해 보세요.");
        if (warnIds.Contains("below_jedec"))
            return new("warn", title, "기본 속도보다 느림", "서로 다른 램을 섞었거나 메인보드 제한일 수 있습니다.");

        double? eff = null;
        string spec = "";
        if (info is JsonElement i && i.TryGetProperty("memory", out var mem))
        {
            var mods = mem.GetProperty("modules").EnumerateArray().Where(m => m.GetProperty("size_bytes").GetInt64() > 0).ToList();
            int ch = mem.GetProperty("channels").GetInt32();
            int speed = mods.Count > 0 ? mods.Min(m => m.GetProperty("configured_speed").GetInt32()) : 0;
            spec = mods.Count > 0 ? $"{(ch == 2 ? "듀얼 채널" : ch == 1 ? "싱글 채널" : $"{ch}채널")} · {mods[0].GetProperty("type").GetString()} {speed}" : "";
            double theory = speed * 8.0 * ch / 1000.0;  // MT/s × 8바이트 × 채널 = MB/s → GB/s
            var bw = score?.GetProperty("items").EnumerateArray().FirstOrDefault(x => x.GetProperty("key").GetString() == "mem_bw_mt");
            if (theory > 0 && bw is JsonElement b && b.ValueKind == JsonValueKind.Object) eff = b.GetProperty("value").GetDouble() / theory;
        }
        if (eff is double e)
            return e < MemEfficiencyWarn
                ? new("warn", title, $"이론 속도의 {e * 100:0}%", $"{spec}. 기대보다 느립니다. 백그라운드 프로그램을 닫고 다시 재 보세요.")
                : new("ok", title, $"이론 속도의 {e * 100:0}%", $"{spec}. 제 속도를 내고 있습니다.");
        if (check is null) return new("none", title, "아직 안 잼", "채널 구성과 동작 속도, 실제 읽기 속도를 봅니다.");
        return new("ok", title, "구성 정상", $"{spec}. 실제 속도는 '점수 측정'에서 확인합니다.");
    }

    private static SummaryCard Background(JsonElement? check, HashSet<string> warnIds)
    {
        const string title = "쉴 때 CPU 사용";
        if (check is not JsonElement c) return new("none", title, "아직 안 잼", "아무것도 안 할 때 다른 프로그램이 CPU를 얼마나 쓰는지 봅니다.");
        var bg = c.GetProperty("background");
        double busy = bg.GetProperty("busy_percent").GetDouble();
        string top = bg.GetProperty("top").EnumerateArray().Select(p => p.GetProperty("name").GetString()).FirstOrDefault() ?? "";
        return warnIds.Contains("background")
            ? new("warn", title, $"{busy:0.0}% 사용 중", $"가장 많이 쓰는 프로그램: {top}. 측정 점수가 그만큼 낮게 나올 수 있습니다.")
            : new("ok", title, $"{busy:0.0}%", "거의 쉬고 있습니다.");
    }

    private static SummaryCard Power(JsonElement? check, HashSet<string> warnIds)
    {
        const string title = "전원 설정";
        if (check is null) return new("none", title, "아직 안 잼", "절전 모드처럼 성능을 막는 설정이 있는지 봅니다.");
        if (warnIds.Contains("on_battery")) return new("warn", title, "배터리 사용 중", "충전기를 연결하세요. 배터리에서는 CPU가 느려집니다.");
        if (warnIds.Contains("power_saver")) return new("warn", title, "절전 모드", "전원 관리 옵션을 '균형 조정'이나 '고성능'으로 바꾸세요.");
        if (warnIds.Contains("efficiency_mode")) return new("warn", title, "최고의 전원 효율", "전원 모드를 '균형' 또는 '최고 성능'으로 바꾸세요.");
        if (warnIds.Contains("max_state")) return new("warn", title, "CPU 속도 제한", "최대 프로세서 상태가 100%가 아닙니다. 전원 옵션 고급 설정을 확인하세요.");
        return new("ok", title, "정상", "성능을 막는 설정이 없습니다.");
    }

    /// <summary>점수를 말로: 고정 기준값 대비 몇 배인지</summary>
    public static string ScoreInWords(double single, double multi) =>
        $"싱글은 프로그램 하나를 돌릴 때, 멀티는 여러 일을 한꺼번에 할 때의 빠르기입니다. 기준값(1000)보다 싱글 {single / 1000:0.00}배, 멀티 {multi / 1000:0.00}배.";
}
