#include "hermes_protocol.h"
#include "board.h"
#include "application.h"
#include "settings.h"
#include "audio/audio_service.h"
#include "display.h"

#include <esp_log.h>
#include <cstring>
#include <mbedtls/base64.h>

#define TAG "HERMES"

// Base64 encoding using mbedtls
static std::string Base64Encode(const uint8_t* data, size_t length) {
    size_t dlen = 0, olen = 0;
    mbedtls_base64_encode(nullptr, 0, &dlen, data, length);
    std::string result(dlen, 0);
    mbedtls_base64_encode((unsigned char*)result.data(), result.size(), &olen, data, length);
    return result;
}

HermesProtocol::HermesProtocol() {
    Settings settings("hermes", false);
    hermes_url_ = settings.GetString("llm_url");
    whisper_url_ = settings.GetString("stt_url");
    tts_url_ = settings.GetString("tts_url");

    // Default URLs if not configured
    if (hermes_url_.empty()) {
        hermes_url_ = "http://100.78.111.3:8090";
    }
    if (whisper_url_.empty()) {
        whisper_url_ = "http://100.78.111.3:5000";
    }
    if (tts_url_.empty()) {
        tts_url_ = "http://100.78.111.3:8080";
    }

    ESP_LOGI(TAG, "Hermes URLs - LLM: %s, STT: %s, TTS: %s",
             hermes_url_.c_str(), whisper_url_.c_str(), tts_url_.c_str());
}

HermesProtocol::~HermesProtocol() {
}

bool HermesProtocol::Start() {
    ESP_LOGI(TAG, "HermesProtocol starting...");
    error_occurred_ = false;
    return true;
}

bool HermesProtocol::OpenAudioChannel() {
    ESP_LOGI(TAG, "Opening audio channel...");
    error_occurred_ = false;
    audio_channel_opened_ = true;
    pending_audio_.clear();

    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }

    return true;
}

void HermesProtocol::CloseAudioChannel(bool send_goodbye) {
    ESP_LOGI(TAG, "Closing audio channel");
    audio_channel_opened_ = false;
    pending_audio_.clear();

    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool HermesProtocol::IsAudioChannelOpened() const {
    return audio_channel_opened_ && !error_occurred_;
}

bool HermesProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!audio_channel_opened_ || error_occurred_) {
        return false;
    }

    // Accumulate audio data
    if (pending_audio_.size() + packet->payload.size() > MAX_PENDING_AUDIO_SIZE) {
        // Buffer full, process pending audio
        if (!pending_audio_.empty()) {
            std::string text = SpeechToText(pending_audio_);
            pending_audio_.clear();

            if (!text.empty() && text != "Error: No speech detected") {
                ESP_LOGI(TAG, "STT result: %s", text.c_str());

                // Get LLM response
                std::string response = ChatWithLLM(text);
                if (!response.empty() && response.find("Error:") != 0) {
                    ESP_LOGI(TAG, "LLM response: %s", response.c_str());

                    // Show thinking emotion on display
                    auto display = Board::GetInstance().GetDisplay();
                    if (display) {
                        display->SetEmotion("thinking");
                    }

                    // Get TTS audio
                    std::vector<uint8_t> audio = TextToSpeech(response);
                    if (!audio.empty()) {
                        // Push audio to be played
                        if (on_incoming_audio_) {
                            auto tts_packet = std::make_unique<AudioStreamPacket>();
                            tts_packet->sample_rate = 16000;
                            tts_packet->frame_duration = 60;
                            tts_packet->timestamp = 0;
                            tts_packet->payload = std::move(audio);
                            on_incoming_audio_(std::move(tts_packet));
                        }
                    }
                }
            }
        }
    }

    // Add new packet to buffer
    pending_audio_.insert(pending_audio_.end(),
                          packet->payload.begin(),
                          packet->payload.end());

    return true;
}

std::string HermesProtocol::SpeechToText(const std::vector<uint8_t>& audio_data) {
    if (audio_data.empty()) {
        return "";
    }

    // Prepare JSON body with base64 encoded audio
    cJSON* root = cJSON_CreateObject();
    std::string base64_audio = Base64Encode(audio_data.data(), audio_data.size());
    cJSON_AddStringToObject(root, "audio", base64_audio.c_str());

    char* json_str = cJSON_PrintUnformatted(root);
    std::string request_body(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    std::string response = PostJson(whisper_url_ + "/inference", request_body);
    if (response.empty()) {
        return "Error: STT request failed";
    }

    // Parse JSON response
    auto json_root = cJSON_Parse(response.c_str());
    if (!json_root) {
        return "Error: Failed to parse STT response";
    }

    cJSON* text_item = cJSON_GetObjectItem(json_root, "text");
    std::string result;
    if (text_item && cJSON_IsString(text_item)) {
        result = text_item->valuestring;
    }

    cJSON_Delete(json_root);
    return result;
}

std::string HermesProtocol::ChatWithLLM(const std::string& text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "message", text.c_str());

    char* json_str = cJSON_PrintUnformatted(root);
    std::string request_body(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    std::string response = PostJson(hermes_url_ + "/chat", request_body);
    if (response.empty()) {
        return "Error: LLM request failed";
    }

    // Parse JSON response - Hermes API returns { "response": "..." }
    auto json_root = cJSON_Parse(response.c_str());
    if (!json_root) {
        return "Error: Failed to parse LLM response";
    }

    cJSON* resp_item = cJSON_GetObjectItem(json_root, "response");
    std::string result;
    if (resp_item && cJSON_IsString(resp_item)) {
        result = resp_item->valuestring;
    }

    cJSON_Delete(json_root);
    return result;
}

std::vector<uint8_t> HermesProtocol::TextToSpeech(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "text", text.c_str());
    cJSON_AddStringToObject(root, "voice", "zh-CN-XiaoxiaoNeural");

    char* json_str = cJSON_PrintUnformatted(root);
    std::string request_body(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    return PostJsonBinary(tts_url_ + "/tts", request_body);
}

bool HermesProtocol::SendText(const std::string& text) {
    // HermesProtocol doesn't use text messages in the same way
    // The text flow is: Audio -> STT -> LLM -> TTS -> Audio
    (void)text;
    return true;
}

bool HermesProtocol::InitHttp() {
    // HTTP is initialized on-demand via Board's network interface
    return true;
}

std::string HermesProtocol::PostJson(const std::string& url, const std::string& body, bool is_binary) {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available");
        return "";
    }

    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return "";
    }

    http->SetTimeout(30000);
    http->SetHeader("Content-Type", "application/json");

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection to %s, error=%d",
                 url.c_str(), http->GetLastError());
        return "";
    }

    // Write body
    if (!body.empty()) {
        int written = http->Write(body.data(), body.size());
        if (written != (int)body.size()) {
            ESP_LOGE(TAG, "Failed to write HTTP body");
            http->Close();
            return "";
        }
    }

    // Read response
    std::string response = http->ReadAll();
    int status = http->GetStatusCode();
    http->Close();

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP request failed with status %d", status);
        return "";
    }

    return response;
}

std::vector<uint8_t> HermesProtocol::PostJsonBinary(const std::string& url, const std::string& body) {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available");
        return {};
    }

    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return {};
    }

    http->SetTimeout(30000);
    http->SetHeader("Content-Type", "application/json");

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection to %s, error=%d",
                 url.c_str(), http->GetLastError());
        return {};
    }

    // Write body
    if (!body.empty()) {
        int written = http->Write(body.data(), body.size());
        if (written != (int)body.size()) {
            ESP_LOGE(TAG, "Failed to write HTTP body");
            http->Close();
            return {};
        }
    }

    // Read binary response
    std::vector<uint8_t> response;
    char buffer[1024];
    int read;
    while ((read = http->Read(buffer, sizeof(buffer))) > 0) {
        response.insert(response.end(), buffer, buffer + read);
    }

    int status = http->GetStatusCode();
    http->Close();

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP request failed with status %d", status);
        return {};
    }

    return response;
}