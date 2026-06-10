namespace SmartAC_API.Interfaces;

public interface IQStashService
{
    Task<bool> ScheduleActionAsync(string action, int delayMinutes);
}
