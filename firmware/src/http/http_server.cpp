#include "http_server.h"
#include "controllers/save.h"
#include "controllers/tools.h"
#include "controllers/config.h"
#include "controllers/status.h"
#include "controllers/health.h"
#include "controllers/soc.h"
#include "controllers/wifi.h"
#include "controllers/blacklist.h"
#include "controllers/ota.h"
#include "controllers/logs.h"
#include "controllers/at_bridge_api.h"
#include "controllers/messages.h"
#include "controllers/sim_expiry.h"
#include "config/config.h"
#include "../logger/logger.h"
#include <LittleFS.h>

// Auth whitelist: routes that require NO authentication
static const char* const AUTH_WHITELIST[] = {"/api/health", nullptr};

static AsyncAuthenticationMiddleware g_authMiddleware;

// 自己拼 401 响应，而不用库的 requestAuthentication()：
// 库默认发 "text/html"（不带 charset），浏览器会按 Latin-1 解析，
// 中文提示「请输入账号密码」在页面上会显示成乱码。
static void sendAuthChallenge(AsyncWebServerRequest* req) {
  const char* body =
      "<!doctype html><html><head><meta charset=\"utf-8\"></head>"
      "<body style=\"font-family:sans-serif;padding:24px\">"
      "请输入账号密码后继续访问（默认 admin / admin123）。"
      "</body></html>";
  AsyncWebServerResponse* resp = req->beginResponse(401, "text/html; charset=utf-8", body);
  resp->addHeader("WWW-Authenticate", "Basic realm=\"SMS Forwarding\"");
  req->send(resp);
}

static AsyncMiddlewareFunction g_conditionalAuth([](AsyncWebServerRequest* req, ArMiddlewareNext next) {
  const char* url = req->url().c_str();
  for (int i = 0; AUTH_WHITELIST[i] != nullptr; ++i) {
    if (strcmp(url, AUTH_WHITELIST[i]) == 0) { next(); return; }
  }
  if (!g_authMiddleware.allowed(req)) {
    sendAuthChallenge(req);
    return;
  }
  next();
});

void HttpServer::refreshAuthCredentials() {
  g_authMiddleware.setUsername(config.webUser.c_str());
  g_authMiddleware.setPassword(config.webPass.c_str());
  g_authMiddleware.generateHash();
  LOG("HTTP", "认证凭证已更新");
}

void HttpServer::setup(AsyncWebServer& server) {
  g_authMiddleware.setUsername(config.webUser.c_str());
  g_authMiddleware.setPassword(config.webPass.c_str());
  g_authMiddleware.setAuthType(AsyncAuthType::AUTH_BASIC);
  g_authMiddleware.setRealm("SMS Forwarding");
  g_authMiddleware.generateHash();

  server.addMiddleware(&g_conditionalAuth);

  // API routes (new)
  server.on("/api/config/export", HTTP_GET, configExportController);
  server.on("/api/config/import", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    configImportController);
  server.on("/api/config/detail",  HTTP_GET,  configController);
  server.on("/api/status",  HTTP_GET,  statusController);
  server.on("/api/health",  HTTP_GET,  healthController);
  server.on("/api/logs",    HTTP_GET,    logsController);
  server.on("/api/logs",    HTTP_DELETE, logsDeleteController);
  server.on("/api/soc",     HTTP_GET,  socController);

  // WiFi configuration API
  // 注意：ESPAsyncWebServer 的字符串 URI 默认按「前缀」匹配，/api/wifi 会吃掉
  // /api/wifi/scan，所以扫描接口必须用精确匹配，并且注册在前面。
  server.on(AsyncURIMatcher::exact("/api/wifi/scan"), HTTP_GET, wifiScanController);
  server.on("/api/wifi", HTTP_GET, wifiGetController);
  server.on("/api/wifi", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    wifiPostController);

  // 设备自身热点（配网兜底）
  server.on("/api/hotspot", HTTP_GET, hotspotGetController);
  server.on("/api/hotspot", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    hotspotPostController);

  // 收件箱（已收到的短信/来电）
  server.on("/api/messages", HTTP_GET,    messagesGetController);
  server.on("/api/messages", HTTP_DELETE, messagesDeleteController);

  // SIM 卡到期提醒（可保存任意号码）
  server.on("/api/sim-expiry", HTTP_GET,    simExpiryGetController);
  server.on("/api/sim-expiry", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    simExpiryPostController);
  server.on("/api/sim-expiry", HTTP_DELETE, simExpiryDeleteController);

  // Blacklist API
  server.on("/api/blacklist", HTTP_GET, blacklistGetController);
  server.on("/api/blacklist", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    blacklistPostController);

  // Configuration save
  server.on("/api/save", HTTP_POST, saveController);
  server.on("/api/reboot", HTTP_POST, saveRebootController);

  // Tool handlers
  server.on("/sendsms", HTTP_POST, sendSmsController);
  server.on("/ping",    HTTP_POST, pingController);
  server.on("/query",   HTTP_GET,  queryController);
  server.on("/flight",  HTTP_GET,  flightModeController);
  server.on("/at",      HTTP_GET,  atCommandController);

  // 远程 AT 桥（Simplus internal/atremote 的对端）。会话期间 /at 与 /ping 被拒绝，
  // 以保证 APDU 序列不被网页端插入的命令破坏。契约见 Simplus docs/remote-at-bridge.md。
  server.on("/at/session", HTTP_POST, atBridgeSessionOpenController);
  server.on("/at/session", HTTP_DELETE,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    atBridgeSessionCloseController);
  server.on("/at/command", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    atBridgeCommandController);
  server.on("/events/calls", HTTP_GET, callEventsController);
  server.on("/at/exchange", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    atBridgeExchangeController);

  // Config reset / reboot API
  server.on("/api/tools/reset-token", HTTP_GET, resetTokenController);
  server.on("/api/tools/reset", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    resetConfigController);
  server.on("/api/tools/reboot", HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    rebootController);
  server.on("/api/tools/coredump/info",   HTTP_GET, coredumpInfoController);
  server.on("/api/tools/coredump/export", HTTP_GET, exportCoreDumpController);

  // OTA upgrade API
  server.on("/api/ota/status",  HTTP_GET,  otaStatusController);
  server.on("/api/ota/version", HTTP_GET,  otaVersionController);
  server.on("/api/ota/start",   HTTP_POST,
    [](AsyncWebServerRequest* request) {},
    nullptr,
    otaStartController);
  server.on("/api/ota/upload",  HTTP_POST,
    otaUploadCompleteController,
    otaUploadChunkController,
    nullptr);
  server.on("/api/ota/upload-fs", HTTP_POST,
    otaUploadFsCompleteController,
    otaUploadFsChunkController,
    nullptr);

  // Suppress LittleFS error logs for common browser auto-requests
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(204);
  });

  // Static pages — served from LittleFS as gzip, browser decompresses automatically
  server.on("/tools", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* resp = request->beginResponse(LittleFS, "/tools.html.gz", "text/html");
    resp->addHeader("Content-Encoding", "gzip");
    request->send(resp);
  });
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* resp = request->beginResponse(LittleFS, "/index.html.gz", "text/html");
    resp->addHeader("Content-Encoding", "gzip");
    request->send(resp);
  });

  // All unmatched routes → 404 (no LittleFS lookup, no VFS error logs)
  server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Not Found");
  });

  server.begin();

  LOG("HTTP", "HTTP服务器已启动");
}
