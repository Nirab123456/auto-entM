// web-server/main.cpp
#include <drogon/drogon.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp> // optional, Drogon has Json::Value or you can use Json::Value

using namespace drogon;

// If running as a separate process:
// - maintain internal copy of controllable values and status.
// If running in the same process you can instead extern these from the receiver.
// Example (embedding): extern std::atomic<double> g_gain;

static std::atomic<double> g_gain_local{1.0};
static std::atomic<uint32_t> g_last_seq_local{0};
static std::atomic<uint64_t> g_highest_sample_local{0};
static std::atomic<int> g_samples_written_local{0};
static std::atomic<bool> g_running_local{true};

static std::mutex ws_clients_mtx;
static std::vector<drogon::WebSocketConnectionPtr> ws_clients;

// Helper: create JSON object for status
Json::Value makeStatusJson() {
    Json::Value j;
    j["running"] = g_running_local.load() ? 1 : 0;
    j["gain"] = g_gain_local.load();
    j["last_seq"] = (Json::UInt)g_last_seq_local.load();
    j["highest_sample_index"] = (Json::UInt64)g_highest_sample_local.load();
    j["samples_written"] = g_samples_written_local.load();
    return j;
}

int main() {
    // Configure log level and port if needed
    // Load drogon config from config.json or programmatically:
    drogon::app().addListener("0.0.0.0", 8080); // HTTP port 8080 (change as needed)

    // Serve static files by mapping routes
    // Map / -> index.html
    drogon::app().registerHandler("/", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        auto resp = HttpResponse::newFileResponse("static/index.html");
        callback(resp);
    }, {Get});

    // Serve JS and CSS explicitly (or use a static file handler)
    drogon::app().registerHandler("/app.js", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        auto resp = HttpResponse::newFileResponse("static/app.js");
        resp->addHeader("Content-Type", "application/javascript");
        callback(resp);
    }, {Get});
    drogon::app().registerHandler("/styles.css", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        auto resp = HttpResponse::newFileResponse("static/styles.css");
        resp->addHeader("Content-Type", "text/css");
        callback(resp);
    }, {Get});

    // Simple GET /status -> returns JSON
    drogon::app().registerHandler("/status", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        Json::Value st = makeStatusJson();
        auto resp = HttpResponse::newHttpJsonResponse(st);
        callback(resp);
    }, {Get});

    // POST /control -> accepts JSON {"gain": <number>}
    drogon::app().registerHandler("/control", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        // Try to parse JSON body
        try {
            auto json = req->getJsonObject();
            if (json && (*json).isMember("gain")) {
                double newGain = (*json)["gain"].asDouble();
                // Optionally validate range:
                if (newGain < 0.1) newGain = 0.1;
                if (newGain > 8.0) newGain = 8.0;
                g_gain_local.store(newGain);
                // respond with new status
                Json::Value j = makeStatusJson();
                auto resp = HttpResponse::newHttpJsonResponse(j);
                callback(resp);
                return;
            }
        } catch (...) {
            // fall through to bad request
        }
        auto bad = HttpResponse::newHttpResponse();
        bad->setStatusCode(k400BadRequest);
        bad->setBody("Invalid JSON or missing 'gain'");
        callback(bad);
    }, {Post});

    // WebSocket at /ws: new connection callback + message callback
    drogon::app().registerWebSocket("/ws",
        // on new connection
        [](const HttpRequestPtr &req, const WebSocketConnectionPtr &wsConn){
            {
                std::lock_guard<std::mutex> lk(ws_clients_mtx);
                ws_clients.push_back(wsConn);
            }
            // Send immediate status update
            Json::Value st = makeStatusJson();
            wsConn->send(st.toStyledString());
        },
        // on message from client
        [](const WebSocketConnectionPtr &wsConn, std::string &&message, const WebSocketMessageType &type){
            // Expect JSON either {"cmd":"set", "gain": 1.5} or {"cmd":"ping"}
            try {
                Json::CharReaderBuilder b;
                Json::Value doc;
                std::string errs;
                std::istringstream is(message);
                if (Json::parseFromStream(b, is, &doc, &errs)) {
                    if (doc.isMember("cmd") && doc["cmd"].asString() == "set" && doc.isMember("gain")) {
                        double newGain = doc["gain"].asDouble();
                        if (newGain < 0.1) newGain = 0.1;
                        if (newGain > 8.0) newGain = 8.0;
                        g_gain_local.store(newGain);
                        // broadcast new status back to all clients (will be done by periodic thread too)
                        Json::Value st = makeStatusJson();
                        std::string s = st.toStyledString();
                        std::lock_guard<std::mutex> lk(ws_clients_mtx);
                        for (auto &c : ws_clients) {
                            if (c && c->connected()) c->send(s);
                        }
                    }
                }
            } catch(...) { /* ignore parse errors */ }
        }
    );

    // Background broadcaster thread: sends current status to all webSocket clients periodically
    std::thread broadcaster([](){
        while (true) {
            if (!g_running_local.load()) break;
            Json::Value st = makeStatusJson();
            std::string s = st.toStyledString();
            {
                std::lock_guard<std::mutex> lk(ws_clients_mtx);
                for (auto it = ws_clients.begin(); it != ws_clients.end();) {
                    auto &c = *it;
                    if (!c || !c->connected()) {
                        it = ws_clients.erase(it);
                    } else {
                        try { c->send(s); } catch(...) { /* ignore per-conn errors */ }
                        ++it;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    // Start Drogon
    drogon::app().run();

    // Cleanup
    g_running_local.store(false);
    if (broadcaster.joinable()) broadcaster.join();
    return 0;
}
