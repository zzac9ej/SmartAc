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
            var success = await qstashService.ScheduleActionAsync(request.Action, request.DelayMinutes);

            if (success)
            {
                return Results.Ok(new { Message = $"冷氣排程已設定！將在 {request.DelayMinutes} 分鐘後執行 {request.Action}。" });
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
