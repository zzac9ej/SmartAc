namespace SmartAC_API.Interfaces;

public interface IQStashService
{
    Task<string?> ScheduleActionAsync(string action, DateTime? targetTimeUtc, int? temperature = null);
    Task<bool> CancelMessageAsync(string messageId);
}
