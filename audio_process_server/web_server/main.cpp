// web-server/main.cpp
#include <drogon/drogon.h>
#include <json/json.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <sstream>
#include <iostream>

using namespace drogon;

// Local copy of the controllable values / status (separate-process mode)
static std::atomic<double> g_gain_local{1.0};
static std::atomic<uint32_t> g_last_seq_local{0};
static std::atomic<uint64_t> g_highest_sample_local{0};
static std::atomic<int> g_samples_written_local{0};
static std::atomic<bool> g_running_local{true};

static std::mutex state_mutex;

// Helper: create JSON object for status
Json::Value makeStatusJson()
{
    Json::Value j;
    j["running"] = g_running_local.load() ? 1 : 0;
    j["gain"] = g_gain_local.load();
    j["last_seq"] = static_cast<Json::UInt>(g_last_seq_local.load());
    j["highest_sample_index"] = static_cast<Json::UInt64>(g_highest_sample_local.load());
    j["samples_written"] = g_samples_written_local.load();
    return j;
}

int main()
{
    // Configure drogon to listen on 0.0.0.0:8080 (or change as needed)
    drogon::app().addListener("0.0.0.0", 8080);

    // Serve index.html at /
    drogon::app().registerHandler("/", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        // Return the static file (relative to working dir)
        auto resp = HttpResponse::newFileResponse("static/index.html");
        callback(resp);
    }, {Get});

    // Serve static JS
    drogon::app().registerHandler("/app.js", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        auto resp = HttpResponse::newFileResponse("static/app.js");
        resp->addHeader("Content-Type", "application/javascript");
        callback(resp);
    }, {Get});

    // Serve static CSS
    drogon::app().registerHandler("/styles.css", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        auto resp = HttpResponse::newFileResponse("static/styles.css");
        resp->addHeader("Content-Type", "text/css");
        callback(resp);
    }, {Get});

    // GET /status -> return JSON status
    drogon::app().registerHandler("/status", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        Json::Value st = makeStatusJson();
        auto resp = HttpResponse::newHttpJsonResponse(st);
        callback(resp);
    }, {Get});

    // POST /control -> accept JSON {"gain": <number>} and update g_gain_local
    drogon::app().registerHandler("/control", [](const HttpRequestPtr &req, std::function<void (const HttpResponsePtr &)> &&callback){
        // Expect a JSON body
        try {
            auto jsonPtr = req->getJsonObject();
            if (!jsonPtr) {
                auto bad = HttpResponse::newHttpResponse();
                bad->setStatusCode(k400BadRequest);
                bad->setBody("Invalid JSON");
                callback(bad);
                return;
            }
            Json::Value &j = *jsonPtr;
            if (!j.isMember("gain")) {
                auto bad = HttpResponse::newHttpResponse();
                bad->setStatusCode(k400BadRequest);
                bad->setBody("Missing 'gain' field");
                callback(bad);
                return;
            }
            double newGain = j["gain"].asDouble();
            // clamp to reasonable range
            if (newGain < 0.01) newGain = 0.01;
            if (newGain > 16.0) newGain = 16.0;
            g_gain_local.store(newGain);

            // Return the new status as JSON
            Json::Value st = makeStatusJson();
            auto resp = HttpResponse::newHttpJsonResponse(st);
            callback(resp);
            return;
        } catch (...) {
            auto bad = HttpResponse::newHttpResponse();
            bad->setStatusCode(k400BadRequest);
            bad->setBody("Error parsing JSON");
            callback(bad);
            return;
        }
    }, {Post});

    // Optionally start a background thread that could poll receiver state (UDP/file) and update local status.
    // For now it's a simple placeholder that sleeps; replace with actual IPC reading if you use separate process.
    std::thread poller([](){
        while (g_running_local.load()) {
            // Example: poll an IPC source here, update g_last_seq_local, etc.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    // start the drogon event loop (blocking)
    drogon::app().run();

    // On exit, stop poller
    g_running_local.store(false);
    if (poller.joinable()) poller.join();

    return 0;
}
