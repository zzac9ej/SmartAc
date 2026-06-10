using Microsoft.AspNetCore.Mvc;
using SmartAC_API.Dtos;
using SmartAC_API.Interfaces;

namespace SmartAC_API.Endpoints;

public static class AcEndpoints
{
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

            var success = await qstashService.ScheduleActionAsync(request.Action, finalDelayMinutes);

            if (success)
            {
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
    }
}
