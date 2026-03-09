#pragma once

#include <QString>

enum class AIProvider { OpenAI, Gemini };

struct AIGenerationRequest
{
  AIProvider provider{AIProvider::OpenAI};
  QString apiKey;
  QString model;
  QString reasoningMode;
  QString permissionMode;
  QString bearerAccessToken;
  QString googleProjectId;
  QString prompt;
  QString filePath;
  QString documentText;
  QString selectedText;
  bool includeDocument{true};
  bool includeSelection{true};
};

struct AIGenerationResult
{
  bool success{false};
  QString code;
  QString rawText;
  QString error;
};

class AICodeGenerator
{
public:
  static AIGenerationResult generate(const AIGenerationRequest& request);
};
