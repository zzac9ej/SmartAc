using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;
using Microsoft.IdentityModel.Tokens;
using System.IdentityModel.Tokens.Jwt;
using System.Security.Cryptography;
using System.Text;

namespace SmartAC_API.Filters;

public class QStashVerifyMiddleware
{
    private readonly RequestDelegate _next;
    private readonly string _currentSigningKey;
    private readonly string _nextSigningKey;
    private readonly ILogger<QStashVerifyMiddleware> _logger;

    public QStashVerifyMiddleware(RequestDelegate next, IConfiguration config, ILogger<QStashVerifyMiddleware> logger)
    {
        _next = next;
        _currentSigningKey = config["QStash:CurrentSigningKey"] ?? "";
        _nextSigningKey = config["QStash:NextSigningKey"] ?? "";
        _logger = logger;
    }

    public async Task InvokeAsync(HttpContext context)
    {
        // 只有 callback 路由需要驗證
        if (!context.Request.Path.StartsWithSegments("/api/callback", StringComparison.OrdinalIgnoreCase))
        {
            await _next(context);
            return;
        }

        if (!context.Request.Headers.TryGetValue("Upstash-Signature", out var signatureHeader))
        {
            _logger.LogWarning("Missing Upstash-Signature header.");
            context.Response.StatusCode = 401;
            await context.Response.WriteAsync("Missing signature.");
            return;
        }

        context.Request.EnableBuffering();
        using var reader = new StreamReader(context.Request.Body, Encoding.UTF8, leaveOpen: true);
        var bodyString = await reader.ReadToEndAsync();
        context.Request.Body.Position = 0;

        var token = signatureHeader.ToString();

        try
        {
            var (isValid, errorDetail) = VerifySignatureWithDetail(token, bodyString, _currentSigningKey);
            if (isValid)
            {
                await _next(context);
                return;
            }

            var (isValidNext, errorDetailNext) = VerifySignatureWithDetail(token, bodyString, _nextSigningKey);
            if (isValidNext)
            {
                await _next(context);
                return;
            }

            _logger.LogWarning("Invalid Upstash signature. CurrentKeyError: {E1}, NextKeyError: {E2}", errorDetail, errorDetailNext);
            context.Response.StatusCode = 401;
            await context.Response.WriteAsync($"Invalid signature. Error: {errorDetail}");
            return;
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "Error verifying Upstash signature.");
            context.Response.StatusCode = 401;
            await context.Response.WriteAsync($"Signature verification failed. Exception: {ex.Message}");
            return;
        }
    }

    private (bool isValid, string error) VerifySignatureWithDetail(string token, string body, string signingKey)
    {
        if (string.IsNullOrEmpty(signingKey)) return (false, "Signing key is empty.");

        var tokenHandler = new JwtSecurityTokenHandler();
        var key = Encoding.UTF8.GetBytes(signingKey);

        try
        {
            tokenHandler.ValidateToken(token, new TokenValidationParameters
            {
                ValidateIssuerSigningKey = true,
                IssuerSigningKey = new SymmetricSecurityKey(key),
                ValidateIssuer = true,
                ValidIssuer = "Upstash",
                ValidateAudience = false,
                ValidateLifetime = true,
                ClockSkew = TimeSpan.FromMinutes(5)
            }, out var validatedToken);

            var jwtToken = (JwtSecurityToken)validatedToken;

            // Verify body hash
            var bodyClaim = jwtToken.Claims.FirstOrDefault(c => c.Type == "body")?.Value;
            
            using var sha256 = SHA256.Create();
            var hashBytes = sha256.ComputeHash(Encoding.UTF8.GetBytes(body));
            var expectedHash = Base64UrlEncoder.Encode(hashBytes);

            if (string.IsNullOrEmpty(body) && string.IsNullOrEmpty(bodyClaim))
            {
                return (true, "");
            }

            // Upstash 的 body claim 可能會帶有 '=' 結尾 (Padding)，但 .NET 的 Base64UrlEncoder 預設會去掉
            var normalizedBodyClaim = bodyClaim?.TrimEnd('=');

            if (normalizedBodyClaim != expectedHash)
            {
                return (false, $"Body hash mismatch. Expected: {expectedHash}, Got: {bodyClaim}");
            }

            return (true, "");
        }
        catch (Exception ex)
        {
            return (false, $"Token validation exception: {ex.Message}");
        }
    }
}
