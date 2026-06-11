using Microsoft.AspNetCore.Mvc;
using SmartAC_API.Dtos;
using SmartAC_API.Interfaces;
using System.Collections.Concurrent;

namespace SmartAC_API.Endpoints;

public class ScheduledRecord
{
    public string MessageId { get; set; } = string.Empty;
    public string Action { get; set; } = string.Empty;
    public DateTime CreatedAt { get; set; }
    public DateTime ExecuteAt { get; set; }
}

public static class AcEndpoints
{
    // 簡單的記憶體儲存 (伺服器重開會清空，但這對於個人用途已經足夠)
    private static readonly ConcurrentDictionary<string, ScheduledRecord> _schedules = new();

    public static void MapAcEndpoints(this IEndpointRouteBuilder app)
    {
        // 1. iPhone 捷徑呼叫這個端點 (User Input)
        app.MapPost("/api/schedule", async ([FromBody] ScheduleRequestDto request, IQStashService qstashService) =>
        {
            int finalDelayMinutes = 0;

            if (!string.IsNullOrEmpty(request.TargetTime))
            {
                if (TimeSpan.TryParse(request.TargetTime, out var time))
                {
                    // 永遠以台灣時間 (UTC+8) 作為基準，避免不同雲端平台的時區問題
                    var nowTw = DateTime.UtcNow.AddHours(8);
                    var targetDateTime = nowTw.Date.Add(time);
                    
                    if (targetDateTime <= nowTw)
                    {
                        targetDateTime = targetDateTime.AddDays(1); // 明天的這個時間
                    }
                    finalDelayMinutes = (int)Math.Ceiling((targetDateTime - nowTw).TotalMinutes);
                }
                else
                {
                    return Results.BadRequest(new { Message = "TargetTime 格式錯誤，請使用 HH:mm (例如 23:30)" });
                }
            }
            else if (request.DelayHours.HasValue)
            {
                finalDelayMinutes = (int)(request.DelayHours.Value * 60);
            }
            else if (request.DelayMinutes.HasValue)
            {
                finalDelayMinutes = request.DelayMinutes.Value;
            }
            else
            {
                return Results.BadRequest(new { Message = "必須提供 DelayMinutes, DelayHours 或 TargetTime 其中之一" });
            }

            var messageId = await qstashService.ScheduleActionAsync(request.Action, finalDelayMinutes);

            if (!string.IsNullOrEmpty(messageId))
            {
                var record = new ScheduledRecord
                {
                    MessageId = messageId,
                    Action = request.Action,
                    CreatedAt = DateTime.UtcNow.AddHours(8),
                    ExecuteAt = DateTime.UtcNow.AddHours(8).AddMinutes(finalDelayMinutes)
                };
                _schedules.TryAdd(messageId, record);

                return Results.Ok(new { Message = $"冷氣排程已設定！將在 {finalDelayMinutes} 分鐘後執行 {request.Action}。" });
            }
            
            return Results.Problem("QStash API 排程失敗，請查看伺服器 Log。");
        });

        // 2. QStash 倒數結束後，呼叫這個端點 (Callback & 喚醒執行)
        app.MapPost("/api/callback", async ([FromBody] ActionRequestDto request, IMqttService mqttService) =>
        {
            var success = await mqttService.PublishCommandAsync(request.Action);

            if (success)
            {
                return Results.Ok(new { Message = "MQTT 訊號已發送給家中 ESP32" });
            }
            
            return Results.Problem("MQTT 發送失敗，請查看伺服器 Log。");
        });

        // 3. 取得目前所有的排程清單 (給前端 Web App 顯示用)
        app.MapGet("/api/schedules", () =>
        {
            // 自動清除已經過期的排程
            var now = DateTime.UtcNow.AddHours(8);
            var expiredKeys = _schedules.Where(k => k.Value.ExecuteAt < now).Select(k => k.Key).ToList();
            foreach (var key in expiredKeys)
            {
                _schedules.TryRemove(key, out _);
            }

            var list = _schedules.Values.OrderBy(x => x.ExecuteAt).ToList();
            return Results.Ok(list);
        });

        // 4. 取消特定的排程 (給前端 Web App 取消用)
        app.MapDelete("/api/schedule/{messageId}", async (string messageId, IQStashService qstashService) =>
        {
            var success = await qstashService.CancelMessageAsync(messageId);
            
            // 無論 QStash 那邊是不是已經過期找不到，我們本機的紀錄都把它清掉
            _schedules.TryRemove(messageId, out _);

            if (success)
            {
                return Results.Ok(new { Message = "排程已成功取消" });
            }
            return Results.Ok(new { Message = "排程可能已經執行或不存在，本機紀錄已清除" });
        });
    }
}
