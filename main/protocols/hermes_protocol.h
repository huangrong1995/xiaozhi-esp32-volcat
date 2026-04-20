#ifndef HERMES_PROTOCOL_H_
#define HERMES_PROTOCOL_H_

#include "protocol.h"

#include <string>
#include <memory>
#include <cJSON.h>

class HermesProtocol : public Protocol {
public:
    HermesProtocol();
    ~HermesProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    bool SendText(const std::string& text) override;

    // STT - Convert speech to text using Whisper
    std::string SpeechToText(const std::vector<uint8_t>& audio_data);

    // LLM - Get response from Hermes LLM
    std::string ChatWithLLM(const std::string& text);

    // TTS - Convert text to speech using Edge TTS
    std::vector<uint8_t> TextToSpeech(const std::string& text);

    // HTTP request helpers
    bool InitHttp();
    std::string PostJson(const std::string& url, const std::string& body, bool is_binary = false);
    std::vector<uint8_t> PostJsonBinary(const std::string& url, const std::string& body);

    // Settings
    std::string hermes_url_;
    std::string whisper_url_;
    std::string tts_url_;

    // Audio buffer for STT
    std::vector<uint8_t> pending_audio_;
    static constexpr size_t MAX_PENDING_AUDIO_SIZE = 32000;  // ~1 second of 16kHz 16-bit mono

    // State
    bool audio_channel_opened_ = false;
    bool is_processing_ = false;
};

#endif  // HERMES_PROTOCOL_H_