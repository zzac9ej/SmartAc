namespace SmartAC_API.Interfaces;

public interface IMqttService
{
    Task<bool> PublishCommandAsync(string command);
}
