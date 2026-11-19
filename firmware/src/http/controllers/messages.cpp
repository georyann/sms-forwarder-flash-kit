#include "messages.h"
#include "history/history.h"
#include "http/json_response.h"

void messagesGetController(AsyncWebServerRequest* request) {
  // listJson() 直接复用已保存的 JSON 行拼接，不做二次解析
  String body = "{\"ok\":true,\"count\":" + String(History::count()) +
                ",\"max\":" + String(HISTORY_MAX_ITEMS) +
                ",\"messages\":" + History::listJson() + "}";
  request->send(200, "application/json", body);
}

void messagesDeleteController(AsyncWebServerRequest* request) {
  History::clear();
  JsonResp::ok(request, "已清空收到的信息");
}
