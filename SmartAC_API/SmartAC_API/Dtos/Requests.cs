namespace SmartAC_API.Dtos;

public class ScheduleRequestDto
{
    public string Action { get; set; } = string.Empty; // 例如 "turn_on" 或 "turn_off"
    public int DelayMinutes { get; set; } // 多少分鐘後執行
}

public class ActionRequestDto
{
    public string Action { get; set; } = string.Empty;
}
