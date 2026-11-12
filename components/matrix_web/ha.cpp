// Підписки на стани Home Assistant під час роботи (сутність задається з веб-панелі).
//
// ESPHome не вміє відписуватися, тому кожна пара «сутність + атрибут» підписується
// один раз; при повторному watch_ha() лише замінюється обробник. Якщо Home Assistant
// уже підключений, нову підписку оголошуємо йому одразу — інакше вона запрацювала б
// лише після перепідключення (так само зроблено в проєкті panel).
#include "matrix_web.h"

#include <cstring>

#include "esphome/core/log.h"

#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif

namespace esphome::matrix_web {

static const char *const TAG = "matrix_web.ha";

void MatrixWeb::watch_ha(const std::string &entity, const std::string &attribute, HaCallback cb) {
  if (entity.empty())
    return;
  const std::string key = attribute.empty() ? entity : entity + "|" + attribute;
  auto &sub = this->ha_subs_[key];
  if (cb)  // без обробника (інформери) — лише кешуємо значення, чужий обробник не чіпаємо
    sub.cb = std::move(cb);
  if (sub.has_value) {
    if (sub.cb)
      sub.cb(sub.value);  // останнє відоме значення — одразу
    return;
  }
  if (sub.subscribed)
    return;
  sub.subscribed = true;

#ifdef USE_API
  auto *api = api::global_api_server;
  if (api == nullptr) {
    ESP_LOGW(TAG, "API is not available, cannot watch %s", key.c_str());
    return;
  }
  optional<std::string> attr;
  if (!attribute.empty())
    attr = attribute;
  api->subscribe_home_assistant_state(entity, attr, [this, key](StringRef value) {
    auto &s = this->ha_subs_[key];
    s.value = value.str();
    s.has_value = true;
    if (s.cb)
      s.cb(s.value);
  });
  ESP_LOGD(TAG, "Watching %s", key.c_str());

  if (!api->is_connected())
    return;  // підписка буде передана HA під час підключення
  api::SubscribeHomeAssistantStateResponse req;
  req.entity_id = StringRef(entity);
  req.attribute = StringRef(attribute);
  req.once = false;
  for (auto &client : api->active_clients()) {
    if (!client || client->is_marked_for_removal() || !client->is_authenticated())
      continue;
    const char *name = client->get_name();
    if (name == nullptr || strstr(name, "Home Assistant") == nullptr)
      continue;
    (void) client->send_message(req);
  }
#else
  ESP_LOGW(TAG, "Built without API, cannot watch %s", key.c_str());
#endif
}

const std::string *MatrixWeb::ha_value(const std::string &entity, const std::string &attribute) const {
  const std::string key = attribute.empty() ? entity : entity + "|" + attribute;
  auto it = this->ha_subs_.find(key);
  if (it == this->ha_subs_.end() || !it->second.has_value)
    return nullptr;
  return &it->second.value;
}

}  // namespace esphome::matrix_web
