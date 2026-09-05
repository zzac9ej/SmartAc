using Microsoft.AspNetCore.Mvc;
using SmartAC_API.Dtos;
using SmartAC_API.Interfaces;
using SmartAC_API.Models;
using System.Collections.Concurrent;
using System.Text.Json;

namespace SmartAC_API.Endpoints;

public class ScheduledRecord
{
    public string MessageId { get; set; } = string.Empty;
    public string Action { get; set; } = string.Empty;
    public string RoomId { get; set; } = string.Empty;
    public DateTime CreatedAt { get; set; }
    public DateTime ExecuteAt { get; set; }
}

public static class AcEndpoints
{
    // 簡單的記憶體儲存 (伺服器重開會清空，但這對於個人用途已經足夠)
    private static readonly ConcurrentDictionary<string, ScheduledRecord> _schedules = new();

    // ── 伺服器端房間溫度持久化儲存（全員同步） ──
    private static readonly string _stateFilePath = Path.Combine(AppContext.BaseDirectory, "room_states.json");
    private static readonly ConcurrentDictionary<string, int> _roomTemperatures = LoadRoomStates();

    private static ConcurrentDictionary<string, int> LoadRoomStates()
    {
        var dict = new ConcurrentDictionary<string, int>(StringComparer.OrdinalIgnoreCase)
        {
            ["office"] = 25,
            ["home"] = 27
        };
        try
        {
            if (File.Exists(_stateFilePath))
            {
                var json = File.ReadAllText(_stateFilePath);
                var loaded = JsonSerializer.Deserialize<Dictionary<string, int>>(json);
                if (loaded != null)
                {
                    foreach (var kvp in loaded)
                    {
                        dict[kvp.Key] = kvp.Value;
                    }
                }
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[State] Load error: {ex.Message}");
        }
        return dict;
    }

    private static void SaveRoomStates()
    {
        try
        {
            var json = JsonSerializer.Serialize(_roomTemperatures);
            File.WriteAllText(_stateFilePath, json);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[State] Save error: {ex.Message}");
        }
    }

    public static void MapAcEndpoints(this IEndpointRouteBuilder app)
    {
        // ─────────────────────────────────────────────────────────────────────
        // 輔助：從 appsettings 讀取設定
        // ─────────────────────────────────────────────────────────────────────

        // 取得 AuthConfig
        AuthConfig GetAuth(IConfiguration config) => new()
        {
            AdminToken  = config["Auth:AdminToken"]  ?? "",
            GuestToken  = config["Auth:GuestToken"]  ?? "",
            GuestRoomId = config["Auth:GuestRoomId"] ?? "office"
        };

        // 取得所有 Rooms
        List<RoomConfig> GetRooms(IConfiguration config)
        {
            var rooms = new List<RoomConfig>();
            config.GetSection("Rooms").Bind(rooms);
            return rooms;
        }

        // 透過 token 驗證身份，回傳 (role, roomId)；null 代表無效
        (string role, string roomId)? ResolveToken(string token, AuthConfig auth)
        {
            if (token == auth.AdminToken) return ("admin", "");
            if (token == auth.GuestToken) return ("guest", auth.GuestRoomId);
            return null;
        }

        // 透過 roomId 取得該房間的 MQTT Topic
        string? GetMqttTopic(string roomId, List<RoomConfig> rooms) =>
            rooms.FirstOrDefault(r => r.Id == roomId)?.MqttTopic;

        // ─────────────────────────────────────────────────────────────────────
        // 0. 身份驗證端點：前端載入頁面時呼叫，確認 token 角色與目前各房間溫度
        // ─────────────────────────────────────────────────────────────────────
        app.MapGet("/api/auth/me", ([FromQuery] string token, IConfiguration config) =>
        {
            var auth  = GetAuth(config);
            var rooms = GetRooms(config);
            var result = ResolveToken(token, auth);

            if (result == null)
                return Results.Unauthorized();

            var (role, roomId) = result.Value;

            if (role == "guest")
            {
                var roomName = rooms.FirstOrDefault(r => r.Id == roomId)?.Name ?? roomId;
                var temp = _roomTemperatures.GetOrAdd(roomId, r => r.Equals("office", StringComparison.OrdinalIgnoreCase) ? 25 : 27);
                return Results.Ok(new { role, roomId, roomName, temperature = temp });
            }
            else // admin
            {
                var roomList = rooms.Select(r => new { 
                    id = r.Id, 
                    name = r.Name,
                    temperature = _roomTemperatures.GetOrAdd(r.Id, id => id.Equals("office", StringComparison.OrdinalIgnoreCase) ? 25 : 27)
                }).ToList();
                return Results.Ok(new { role, rooms = roomList });
            }
        });

        // ─────────────────────────────────────────────────────────────────────
        // 1. iPhone 捷徑呼叫這個端點 (User Input)
        // ─────────────────────────────────────────────────────────────────────
        app.MapPost("/api/schedule", async (
            [FromBody] ScheduleRequestDto request,
            [FromQuery] string token,
            IQStashService qstashService,
            IConfiguration config) =>
        {
            // ── 身份驗證 ──
            var auth   = GetAuth(config);
            var rooms  = GetRooms(config);
            var result = ResolveToken(token, auth);

            if (result == null)
                return Results.Unauthorized();

            var (role, resolvedRoomId) = result.Value;

            // 確認 roomId：GUEST 鎖定自己的房間，ADMIN 可指定任意房間
            string targetRoomId;
            if (role == "guest")
            {
                // GUEST 不得操作其他房間
                targetRoomId = resolvedRoomId;
                if (!string.IsNullOrEmpty(request.RoomId) && request.RoomId != resolvedRoomId)
                    return Results.Forbid();
            }
            else // admin
            {
                targetRoomId = request.RoomId ?? rooms.FirstOrDefault()?.Id ?? "";
            }

            // 查出該房間的 MQTT Topic（存入排程紀錄，callback 時使用）
            var mqttTopic = GetMqttTopic(targetRoomId, rooms);
            if (mqttTopic == null)
                return Results.BadRequest(new { Message = $"找不到房間 '{targetRoomId}'" });

            // 同步記憶最新溫度
            if (request.Temperature.HasValue && request.Temperature.Value >= 16 && request.Temperature.Value <= 32)
            {
                _roomTemperatures[targetRoomId] = request.Temperature.Value;
                SaveRoomStates();
            }

            // ── 時間計算 ──
            DateTime? targetTimeUtc = null;

            if (!string.IsNullOrEmpty(request.TargetTime))
            {
                if (TimeSpan.TryParse(request.TargetTime, out var time))
                {
                    // 永遠以台灣時間 (UTC+8) 作為基準，避免不同雲端平台的時區問題
                    var nowTw = DateTime.UtcNow.AddHours(8);
                    var targetDateTimeTw = nowTw.Date.Add(time);
                    
                    if (targetDateTimeTw <= nowTw)
                    {
                        targetDateTimeTw = targetDateTimeTw.AddDays(1); // 明天的這個時間
                    }
                    targetTimeUtc = targetDateTimeTw.AddHours(-8);
                }
                else
                {
                    return Results.BadRequest(new { Message = "TargetTime 格式錯誤，請使用 HH:mm (例如 23:30)" });
                }
            }
            else if (request.DelayHours.HasValue)
            {
                targetTimeUtc = DateTime.UtcNow.AddHours(request.DelayHours.Value);
            }
            else if (request.DelayMinutes.HasValue && request.DelayMinutes.Value > 0)
            {
                targetTimeUtc = DateTime.UtcNow.AddMinutes(request.DelayMinutes.Value);
            }
            else
            {
                // 如果是 DelayMinutes = 0 (馬上開)，targetTimeUtc 留空，代表不需延遲
                targetTimeUtc = null;
            }

            // QStash callback body 帶入 RoomId，讓 callback 時知道要打哪個 Topic
            var messageId = await qstashService.ScheduleActionAsync(request.Action, targetTimeUtc, request.Temperature, targetRoomId);

            if (!string.IsNullOrEmpty(messageId))
            {
                var executeTimeUtc = targetTimeUtc ?? DateTime.UtcNow;

                var record = new ScheduledRecord
                {
                    MessageId = messageId,
                    Action    = request.Action,
                    RoomId    = targetRoomId,
                    CreatedAt = DateTime.UtcNow,
                    ExecuteAt = executeTimeUtc
                };
                _schedules.TryAdd(messageId, record);

                // 為了給使用者的回傳訊息，還是算一下台灣時間
                var executeTimeTw = targetTimeUtc.HasValue ? targetTimeUtc.Value.AddHours(8) : DateTime.UtcNow.AddHours(8);
                var msg = targetTimeUtc.HasValue 
                    ? $"冷氣排程已設定！將在 {executeTimeTw:MM/dd HH:mm} 執行 {request.Action}。"
                    : $"指令已成功發送！({request.Action})";

                return Results.Ok(new { Message = msg });
            }
            
            return Results.Problem("QStash API 排程失敗，請查看伺服器 Log。");
        });

        // ─────────────────────────────────────────────────────────────────────
        // 2. QStash 倒數結束後，呼叫這個端點 (Callback & 喚醒執行)
        // ─────────────────────────────────────────────────────────────────────
        app.MapPost("/api/callback", async (
            [FromBody] ActionRequestDto request,
            IMqttService mqttService,
            IConfiguration config) =>
        {
            // Callback body 帶有 RoomId，依此查出正確的 MQTT Topic
            var rooms = GetRooms(config);
            var mqttTopic = !string.IsNullOrEmpty(request.RoomId)
                ? GetMqttTopic(request.RoomId, rooms)
                : null;

            if (!string.IsNullOrEmpty(request.RoomId) && request.Temperature.HasValue && request.Temperature.Value >= 16 && request.Temperature.Value <= 32)
            {
                _roomTemperatures[request.RoomId] = request.Temperature.Value;
                SaveRoomStates();
            }

            var success = await mqttService.PublishCommandAsync(request.Action, request.Temperature, mqttTopic);

            if (success)
            {
                return Results.Ok(new { Message = "MQTT 訊號已發送給家中 ESP32" });
            }
            
            return Results.Problem("MQTT 發送失敗，請查看伺服器 Log。");
        });

        // ─────────────────────────────────────────────────────────────────────
        // 3. 取得目前所有的排程清單 (給前端 Web App 顯示用)
        //    GUEST 只能看自己房間，ADMIN 可看全部
        //    同時在 Header 帶入該房間最新伺服器溫度以進行全員同步
        // ─────────────────────────────────────────────────────────────────────
        app.MapGet("/api/schedules", (
            [FromQuery] string token, 
            [FromQuery] string? roomId, 
            HttpContext httpContext, 
            IConfiguration config) =>
        {
            var auth   = GetAuth(config);
            var result = ResolveToken(token, auth);

            if (result == null)
                return Results.Unauthorized();

            var (role, resolvedRoomId) = result.Value;

            // 自動清除已經過期的排程
            var now = DateTime.UtcNow;
            var expiredKeys = _schedules.Where(k => k.Value.ExecuteAt < now).Select(k => k.Key).ToList();
            foreach (var key in expiredKeys)
                _schedules.TryRemove(key, out _);

            IEnumerable<ScheduledRecord> list = _schedules.Values.OrderBy(x => x.ExecuteAt);

            var targetRoom = role == "guest" ? resolvedRoomId : (roomId ?? "home");
            if (!string.IsNullOrEmpty(targetRoom))
            {
                var currentTemp = _roomTemperatures.GetOrAdd(targetRoom, r => r.Equals("office", StringComparison.OrdinalIgnoreCase) ? 25 : 27);
                httpContext.Response.Headers["X-Room-Temperature"] = currentTemp.ToString();
            }

            // GUEST 只能看自己房間
            if (role == "guest")
                list = list.Where(x => x.RoomId == resolvedRoomId);
            // ADMIN 可用 roomId 過濾，或不帶參數看全部
            else if (!string.IsNullOrEmpty(roomId))
                list = list.Where(x => x.RoomId == roomId);

            return Results.Ok(list.ToList());
        });

        // ─────────────────────────────────────────────────────────────────────
        // 3.1 取得房間即時狀態與溫度
        // ─────────────────────────────────────────────────────────────────────
        app.MapGet("/api/room/status", ([FromQuery] string token, [FromQuery] string? roomId, IConfiguration config) =>
        {
            var auth = GetAuth(config);
            var rooms = GetRooms(config);
            var result = ResolveToken(token, auth);
            if (result == null) return Results.Unauthorized();
            var (role, resolvedRoomId) = result.Value;

            if (role == "guest")
            {
                var temp = _roomTemperatures.GetOrAdd(resolvedRoomId, r => r.Equals("office", StringComparison.OrdinalIgnoreCase) ? 25 : 27);
                return Results.Ok(new { roomId = resolvedRoomId, temperature = temp });
            }
            else
            {
                if (!string.IsNullOrEmpty(roomId))
                {
                    var temp = _roomTemperatures.GetOrAdd(roomId, r => r.Equals("office", StringComparison.OrdinalIgnoreCase) ? 25 : 27);
                    return Results.Ok(new { roomId, temperature = temp });
                }
                var all = rooms.Select(r => new {
                    roomId = r.Id,
                    temperature = _roomTemperatures.GetOrAdd(r.Id, id => id.Equals("office", StringComparison.OrdinalIgnoreCase) ? 25 : 27)
                }).ToList();
                return Results.Ok(all);
            }
        });

        // ─────────────────────────────────────────────────────────────────────
        // 4. 取消特定的排程
        // ─────────────────────────────────────────────────────────────────────
        app.MapDelete("/api/schedule/{messageId}", async (
            string messageId,
            [FromQuery] string token,
            IQStashService qstashService,
            IConfiguration config) =>
        {
            var auth   = GetAuth(config);
            var result = ResolveToken(token, auth);

            if (result == null)
                return Results.Unauthorized();

            var (role, resolvedRoomId) = result.Value;

            // GUEST 只能刪除自己房間的排程
            if (role == "guest")
            {
                if (_schedules.TryGetValue(messageId, out var record) && record.RoomId != resolvedRoomId)
                    return Results.Forbid();
            }

            var success = await qstashService.CancelMessageAsync(messageId);
            
            // 無論 QStash 那邊是不是已經過期找不到，我們本機的紀錄都把它清掉
            _schedules.TryRemove(messageId, out _);

            if (success)
                return Results.Ok(new { Message = "排程已成功取消" });

            return Results.Ok(new { Message = "排程可能已經執行或不存在，本機紀錄已清除" });
        });
    }
}
