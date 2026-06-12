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

    public async Task<string?> ScheduleActionAsync(string action, DateTime? targetTimeUtc, int? temperature = null)
    {
        var qstashToken = _config["QStash:Token"];
        var callbackUrl = _config["QStash:CallbackUrl"];
        var qstashBaseUrl = _config["QStash:BaseUrl"] ?? "https://qstash.upstash.io";
        
        // 確保網址結尾沒有斜線，方便拼接
        qstashBaseUrl = qstashBaseUrl.TrimEnd('/');

        var client = _httpClientFactory.CreateClient();
        client.DefaultRequestHeaders.Add("Authorization", $"Bearer {qstashToken}");
        
        var qstashUrl = $"{qstashBaseUrl}/v2/publish/{callbackUrl}";
        
        object payloadData = temperature.HasValue 
            ? new { Action = action, Temperature = temperature.Value }
            : new { Action = action };
            
        var payload = JsonSerializer.Serialize(payloadData);
        var content = new StringContent(payload);
        content.Headers.ContentType = new MediaTypeHeaderValue("application/json");

        var qstashRequest = new HttpRequestMessage(HttpMethod.Post, qstashUrl)
        {
            Content = content
        };

        if (targetTimeUtc.HasValue)
        {
            var unixTimestamp = new DateTimeOffset(targetTimeUtc.Value).ToUnixTimeSeconds();
            qstashRequest.Headers.Add("Upstash-Not-Before", unixTimestamp.ToString());
        }

        var response = await client.SendAsync(qstashRequest);

        if (!response.IsSuccessStatusCode)
        {
            var error = await response.Content.ReadAsStringAsync();
            _logger.LogError("QStash API Error: {Error}", error);
            throw new Exception($"QStash API 回傳錯誤: {response.StatusCode} - {error}");
        }

        var responseBody = await response.Content.ReadAsStringAsync();
        try
        {
            var json = JsonDocument.Parse(responseBody);
            if (json.RootElement.TryGetProperty("messageId", out var messageIdProp))
            {
                return messageIdProp.GetString();
            }
        }
        catch (JsonException ex)
        {
            _logger.LogError(ex, "Failed to parse QStash response: {ResponseBody}", responseBody);
        }

        return null;
    }

    public async Task<bool> CancelMessageAsync(string messageId)
    {
        var qstashToken = _config["QStash:Token"];
        var qstashBaseUrl = _config["QStash:BaseUrl"] ?? "https://qstash.upstash.io";
        qstashBaseUrl = qstashBaseUrl.TrimEnd('/');

        var client = _httpClientFactory.CreateClient();
        client.DefaultRequestHeaders.Add("Authorization", $"Bearer {qstashToken}");

        var url = $"{qstashBaseUrl}/v2/messages/{messageId}";
        var response = await client.DeleteAsync(url);

        if (!response.IsSuccessStatusCode)
        {
            var error = await response.Content.ReadAsStringAsync();
            _logger.LogError("QStash Delete API Error: {Error}", error);
            return false;
        }

        return true;
    }
}
