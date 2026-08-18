using SmartAC_API.Endpoints;
using SmartAC_API.Filters;
using SmartAC_API.Interfaces;
using SmartAC_API.Services;

var builder = WebApplication.CreateBuilder(args);

// 註冊 HttpClient 與我們自己抽離的 Services
builder.Services.AddHttpClient();
builder.Services.AddScoped<IQStashService, QStashService>();
builder.Services.AddScoped<IMqttService, MqttService>();

var app = builder.Build();

app.UseMiddleware<QStashVerifyMiddleware>();

app.UseDefaultFiles();
app.UseStaticFiles();

// ── 前端頁面路由 ────────────────────────────────────────────────────────────
// GUEST 遙控器頁面：/g/{token}  → guest.html
app.MapGet("/g/{token}", async (HttpContext ctx) =>
{
    ctx.Response.ContentType = "text/html; charset=utf-8";
    var file = Path.Combine(app.Environment.WebRootPath, "guest.html");
    await ctx.Response.SendFileAsync(file);
});

// ADMIN 主控台頁面：/a/{token}  → admin.html
app.MapGet("/a/{token}", async (HttpContext ctx) =>
{
    ctx.Response.ContentType = "text/html; charset=utf-8";
    var file = Path.Combine(app.Environment.WebRootPath, "admin.html");
    await ctx.Response.SendFileAsync(file);
});

// 根路徑顯示「請使用正確連結」提示（index.html 已改成提示頁）
// ────────────────────────────────────────────────────────────────────────────

// 註冊所有的 API Endpoints
app.MapAcEndpoints();

app.Run();
