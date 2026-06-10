using System.Net.Http.Headers;
using System.Text.Json;
using SmartAC_API.Interfaces;

namespace SmartAC_API.Services;

public class QStashService : IQStashService
{
    private readonly IHttpClientFactory _httpClientFactory;
    private readonly IConfiguration _config;
    private readonly ILogger<QStashService> _logger;

    public QStashService(IHttpClientFactory httpClientFactory, IConfiguration config, ILogger<QStashService> logger)
    {
        _httpClientFactory = httpClientFactory;
        _config = config;
        _logger = logger;
    }

    public async Task<bool> ScheduleActionAsync(string action, int delayMinutes)
    {
        var qstashToken = _config["QStash:Token"];
        var callbackUrl = _config["QStash:CallbackUrl"];
        var qstashBaseUrl = _config["QStash:BaseUrl"] ?? "https://qstash.upstash.io";
        
        // 確保網址結尾沒有斜線，方便拼接
        qstashBaseUrl = qstashBaseUrl.TrimEnd('/');

        var client = _httpClientFactory.CreateClient();
        client.DefaultRequestHeaders.Add("Authorization", $"Bearer {qstashToken}");
        
        var qstashUrl = $"{qstashBaseUrl}/v2/publish/{callbackUrl}";
        
        var payload = JsonSerializer.Serialize(new { Action = action });
        var content = new StringContent(payload);
        content.Headers.ContentType = new MediaTypeHeaderValue("application/json");

        var qstashRequest = new HttpRequestMessage(HttpMethod.Post, qstashUrl)
        {
            Content = content
        };

        if (delayMinutes > 0)
        {
            qstashRequest.Headers.Add("Upstash-Delay", $"{delayMinutes}m");
        }

        var response = await client.SendAsync(qstashRequest);

        if (!response.IsSuccessStatusCode)
        {
            var error = await response.Content.ReadAsStringAsync();
            _logger.LogError("QStash API Error: {Error}", error);
            throw new Exception($"QStash API 回傳錯誤: {response.StatusCode} - {error}");
        }

        return true;
    }
}
