# 第一階段：建置 (Build)
FROM mcr.microsoft.com/dotnet/sdk:8.0 AS build
WORKDIR /src

# 複製 csproj 並還原 NuGet 套件
COPY ["SmartAC_API.csproj", "./"]
RUN dotnet restore "SmartAC_API.csproj"

# 複製所有原始碼並進行建置
COPY . .
RUN dotnet publish "SmartAC_API.csproj" -c Release -o /app/publish /p:UseAppHost=false

# 第二階段：執行 (Runtime)
FROM mcr.microsoft.com/dotnet/aspnet:8.0 AS final
WORKDIR /app

# 把第一階段建置好的檔案複製過來
COPY --from=build /app/publish .

# 曝露 API 的預設 Port (通常雲端環境會給 8080)
EXPOSE 8080
ENV ASPNETCORE_URLS=http://+:8080

# 設定啟動點
ENTRYPOINT ["dotnet", "SmartAC_API.dll"]
