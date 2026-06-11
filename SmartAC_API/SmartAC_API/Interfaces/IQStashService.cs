namespace SmartAC_API.Interfaces;

public interface IQStashService
{
    Task<string?> ScheduleActionAsync(string action, DateTime? targetTimeUtc);
    Task<bool> CancelMessageAsync(string messageId);
}
