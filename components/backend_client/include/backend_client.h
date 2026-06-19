#ifndef BACKEND_CLIENT_H
#define BACKEND_CLIENT_H

#include <esp_err.h>
#include <stddef.h>

/**
 * @brief Transcribe the recorded WAV file using Whisper STT API.
 * Uploads the WAV file from SPIFFS to OpenAI Whisper or custom compatible API.
 * 
 * @param api_key OpenAI/compatible API Key. If NULL, tries to read from storage.
 * @param out_text Buffer to store the resulting transcription.
 * @param max_len Size of out_text.
 * @return esp_err_t 
 */
esp_err_t backend_speech_to_text(const char* api_key, char* out_text, size_t max_len);

/**
 * @brief Send text query to DeepSeek Chat completions API.
 * Parses the JSON response to extract the assistant reply.
 * 
 * @param api_key DeepSeek API Key. If NULL, tries to read from storage.
 * @param query Text transcription query.
 * @param out_response Buffer to store the response text.
 * @param max_len Size of out_response.
 * @return esp_err_t 
 */
esp_err_t backend_deepseek_chat(const char* api_key, const char* query, char* out_response, size_t max_len);

/**
 * @brief Streams Text-to-Speech audio from OpenAI directly to I2S via a Ring Buffer.
 *
 * @param api_key OpenAI/compatible API Key. If NULL, tries to read from storage.
 * @param text The text to synthesize.
 * @return esp_err_t ESP_OK if streaming completes successfully.
 */
esp_err_t backend_text_to_speech(const char* api_key, const char *text);

#endif // BACKEND_CLIENT_H
