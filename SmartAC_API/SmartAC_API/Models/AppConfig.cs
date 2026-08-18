namespace SmartAC_API.Models;

/// <summary>
/// 對應 appsettings.json 中的 Auth 區塊
/// </summary>
public class AuthConfig
{
    public string AdminToken { get; set; } = string.Empty;
    public string GuestToken { get; set; } = string.Empty;
    public string GuestRoomId { get; set; } = string.Empty;
}

/// <summary>
/// 對應 appsettings.json 中的 Rooms 陣列元素
/// </summary>
public class RoomConfig
{
    public string Id { get; set; } = string.Empty;
    public string Name { get; set; } = string.Empty;
    public string MqttTopic { get; set; } = string.Empty;
}
