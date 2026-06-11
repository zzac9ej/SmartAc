namespace SmartAC_API.Interfaces;

public interface IQStashService
{
    Task<string?> ScheduleActionAsync(string action, int delayMinutes);
    Task<bool> CancelMessageAsync(string messageId);
}
