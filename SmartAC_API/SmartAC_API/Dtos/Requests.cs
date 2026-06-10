namespace SmartAC_API.Dtos;

public class ScheduleRequestDto
{
    public string Action { get; set; } = string.Empty; // 例如 "turn_on" 或 "turn_off"
    public int? DelayMinutes { get; set; } // 多少分鐘後執行
    public double? DelayHours { get; set; } // 多少小時後執行
    public string? TargetTime { get; set; } // 指定幾點執行，格式 "HH:mm" (台灣時間)
}

public class ActionRequestDto
{
    public string Action { get; set; } = string.Empty;
}
