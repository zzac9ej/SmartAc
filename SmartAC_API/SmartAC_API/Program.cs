using Microsoft.AspNetCore.Mvc;
using SmartAC_API.Dtos;
using SmartAC_API.Interfaces;
using SmartAC_API.Services;

using SmartAC_API.Endpoints;

var builder = WebApplication.CreateBuilder(args);

// 註冊 HttpClient 與我們自己抽離的 Services
builder.Services.AddHttpClient();
builder.Services.AddScoped<IQStashService, QStashService>();
builder.Services.AddScoped<IMqttService, MqttService>();

var app = builder.Build();

app.UseMiddleware<SmartAC_API.Filters.QStashVerifyMiddleware>();

app.UseDefaultFiles();
app.UseStaticFiles();

// 註冊所有的 API Endpoints
app.MapAcEndpoints();

app.Run();
