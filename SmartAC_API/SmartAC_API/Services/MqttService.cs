using System.Text.Json;
using MQTTnet;
using MQTTnet.Client;
using SmartAC_API.Interfaces;

namespace SmartAC_API.Services;

public class MqttService : IMqttService
{
    private readonly IConfiguration _config;
    private readonly ILogger<MqttService> _logger;

    public MqttService(IConfiguration config, ILogger<MqttService> logger)
    {
        _config = config;
        _logger = logger;
    }

    public async Task<bool> PublishCommandAsync(string command)
    {
        var mqttFactory = new MqttFactory();
        using var mqttClient = mqttFactory.CreateMqttClient();

        var broker = _config["MQTT:Broker"] ?? "broker.hivemq.com";
        var port = _config.GetValue<int>("MQTT:Port", 1883);
        var topic = _config["MQTT:Topic"] ?? "home/livingroom/ac";

        var mqttOptions = new MqttClientOptionsBuilder()
            .WithTcpServer(broker, port)
            .WithClientId("SmartAC_API_Cloud_" + Guid.NewGuid().ToString())
            .Build();

        try
        {
            await mqttClient.ConnectAsync(mqttOptions);
            
            var messagePayload = JsonSerializer.Serialize(new { command = command });
            var message = new MqttApplicationMessageBuilder()
                .WithTopic(topic)
                .WithPayload(messagePayload)
                .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtLeastOnce)
                .Build();

            await mqttClient.PublishAsync(message);
            await mqttClient.DisconnectAsync();

            return true;
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "MQTT Publish Error: {Message}", ex.Message);
            return false;
        }
    }
}
