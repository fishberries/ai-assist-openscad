#include "gui/AICodeGenerator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include "gui/Network.h"

namespace {

constexpr auto OPENAI_ENDPOINT = "https://api.openai.com/v1/responses";
constexpr auto GEMINI_ENDPOINT =
  "https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent";
constexpr int NETWORK_TIMEOUT_SECONDS = 120;
constexpr int MAX_OUTPUT_TOKENS = 4096;

AIGenerationResult errorResult(const QString& message)
{
  AIGenerationResult result;
  result.error = message;
  return result;
}

QString providerName(AIProvider provider)
{
  return provider == AIProvider::OpenAI ? "OpenAI" : "Gemini";
}

QString permissionInstructions(const QString& permissionMode)
{
  if (permissionMode == "insert") {
    return "Return only the OpenSCAD snippet that should be inserted at the current cursor position.";
  }
  if (permissionMode == "replace-selection") {
    return "Return only the OpenSCAD snippet that should replace the current selection.";
  }
  if (permissionMode == "replace-document") {
    return "Return the full OpenSCAD document so the current file can be replaced safely.";
  }
  return "Return a draft snippet only. Do not rely on any automatic modification outside the generated code.";
}

QString buildSystemPrompt()
{
  return QStringLiteral(
    "You are an OpenSCAD coding assistant embedded directly into the OpenSCAD editor. "
    "Return valid OpenSCAD source code only. Do not use Markdown fences. "
    "If you must explain a constraint, use short OpenSCAD comments starting with //.");
}

QString buildUserPrompt(const AIGenerationRequest& request)
{
  QStringList sections;
  sections << QStringLiteral("User request:\n%1").arg(request.prompt.trimmed());
  sections << QStringLiteral("Output scope:\n%1").arg(permissionInstructions(request.permissionMode));

  if (!request.filePath.trimmed().isEmpty()) {
    sections << QStringLiteral("Current file:\n%1").arg(request.filePath.trimmed());
  }

  if (request.includeSelection && !request.selectedText.isEmpty()) {
    sections << QStringLiteral("Selected OpenSCAD code:\n<<<SELECTION\n%1\nSELECTION")
                  .arg(request.selectedText);
  }

  if (request.includeDocument && !request.documentText.isEmpty()) {
    sections << QStringLiteral("Current OpenSCAD document:\n<<<DOCUMENT\n%1\nDOCUMENT")
                  .arg(request.documentText);
  }

  return sections.join("\n\n");
}

QString extractErrorMessage(const QByteArray& payload, const QString& fallback)
{
  const auto doc = QJsonDocument::fromJson(payload);
  const auto root = doc.object();
  const auto errorValue = root.value("error");
  if (errorValue.isObject()) {
    const auto errorObject = errorValue.toObject();
    const QString message = errorObject.value("message").toString().trimmed();
    if (!message.isEmpty()) return message;
  }
  return fallback.trimmed().isEmpty() ? QStringLiteral("The provider returned an unknown error.")
                                      : fallback.trimmed();
}

QString summarizeGeminiSafetyRatings(const QJsonArray& ratings)
{
  QStringList summaries;
  for (const auto& ratingValue : ratings) {
    const auto rating = ratingValue.toObject();
    const QString category = rating.value("category").toString().trimmed();
    const QString probability = rating.value("probability").toString().trimmed();
    const bool blocked = rating.value("blocked").toBool();

    if (category.isEmpty()) continue;
    QString summary = category;
    if (!probability.isEmpty()) summary += QStringLiteral("=%1").arg(probability);
    if (blocked) summary += QStringLiteral(" (blocked)");
    summaries << summary;
  }
  return summaries.join(QStringLiteral(", "));
}

QString explainGeminiEmptyResponse(const QJsonObject& root)
{
  const auto promptFeedback = root.value("promptFeedback").toObject();
  const QString promptBlockReason = promptFeedback.value("blockReason").toString().trimmed();
  if (!promptBlockReason.isEmpty()) {
    return QStringLiteral("Gemini blocked the prompt: %1").arg(promptBlockReason);
  }

  const auto candidates = root.value("candidates").toArray();
  if (candidates.isEmpty()) {
    return QStringLiteral("Gemini returned no candidates.");
  }

  const auto candidate = candidates.at(0).toObject();
  const QString finishReason = candidate.value("finishReason").toString().trimmed();
  const QString finishMessage = candidate.value("finishMessage").toString().trimmed();
  const auto candidateSafety = summarizeGeminiSafetyRatings(candidate.value("safetyRatings").toArray());
  const auto parts = candidate.value("content").toObject().value("parts").toArray();

  QStringList details;
  details << QStringLiteral("Gemini returned no text content");
  if (!finishReason.isEmpty()) details << QStringLiteral("finishReason=%1").arg(finishReason);
  if (!finishMessage.isEmpty()) details << finishMessage;
  if (!candidateSafety.isEmpty()) details << QStringLiteral("safety=%1").arg(candidateSafety);
  if (parts.isEmpty()) details << QStringLiteral("candidate parts were empty");

  const auto usage = root.value("usageMetadata").toObject();
  const int promptTokens = usage.value("promptTokenCount").toInt(-1);
  const int totalTokens = usage.value("totalTokenCount").toInt(-1);
  const int thoughtTokens = usage.value("thoughtsTokenCount").toInt(-1);
  if (promptTokens >= 0 || totalTokens >= 0 || thoughtTokens >= 0) {
    QStringList usageBits;
    if (promptTokens >= 0) usageBits << QStringLiteral("prompt=%1").arg(promptTokens);
    if (thoughtTokens >= 0) usageBits << QStringLiteral("thoughts=%1").arg(thoughtTokens);
    if (totalTokens >= 0) usageBits << QStringLiteral("total=%1").arg(totalTokens);
    details << QStringLiteral("tokens(%1)").arg(usageBits.join(QStringLiteral(", ")));
  }

  if (finishReason == QLatin1String("RECITATION")) {
    details << QStringLiteral("Try making the prompt or included document more specific and unique.");
  } else if (finishReason == QLatin1String("SAFETY")) {
    details << QStringLiteral("Check whether the prompt or context triggered Gemini safety filters.");
  } else if (finishReason == QLatin1String("STOP")) {
    details << QStringLiteral("Try Gemini 2.5 Flash or disable full-document context for this request.");
  }

  return details.join(QStringLiteral(". ")) + QStringLiteral(".");
}

QString normalizeCodeResponse(const QString& rawText)
{
  const QString trimmed = rawText.trimmed();
  static const QRegularExpression fencedBlock("```(?:[A-Za-z0-9_+-]+)?\\s*\\n([\\s\\S]*?)\\n```");
  const auto match = fencedBlock.match(trimmed);
  if (match.hasMatch()) {
    return match.captured(1).trimmed();
  }
  return trimmed;
}

QString parseOpenAIResponse(const QByteArray& payload)
{
  const auto document = QJsonDocument::fromJson(payload);
  const auto root = document.object();
  if (root.contains("error")) {
    throw NetworkException(QNetworkReply::ProtocolFailure,
                           extractErrorMessage(payload, QStringLiteral("OpenAI request failed.")));
  }

  QStringList chunks;
  const auto outputs = root.value("output").toArray();
  for (const auto& outputValue : outputs) {
    const auto outputObject = outputValue.toObject();
    if (outputObject.value("type").toString() != "message") continue;

    const auto content = outputObject.value("content").toArray();
    for (const auto& contentValue : content) {
      const auto contentObject = contentValue.toObject();
      const auto type = contentObject.value("type").toString();
      if (type == "output_text" || type == "text") {
        const QString text = contentObject.value("text").toString();
        if (!text.isEmpty()) chunks << text;
      }
    }
  }

  if (chunks.isEmpty()) {
    const QString altText = root.value("output_text").toString();
    if (!altText.isEmpty()) return altText;
  }

  return chunks.join("\n").trimmed();
}

bool isGemini3Model(const QString& model)
{
  return model.trimmed().startsWith("gemini-3", Qt::CaseInsensitive);
}

bool isGeminiProModel(const QString& model)
{
  return model.contains("-pro", Qt::CaseInsensitive);
}

QString geminiThinkingLevel(const QString& reasoningMode)
{
  if (reasoningMode == "off") return "minimal";
  if (reasoningMode == "low") return "low";
  if (reasoningMode == "medium") return "medium";
  if (reasoningMode == "high") return "high";
  return {};
}

int geminiThinkingBudget(const QString& reasoningMode)
{
  if (reasoningMode == "off") return 0;
  if (reasoningMode == "low") return 1024;
  if (reasoningMode == "medium") return 8192;
  if (reasoningMode == "high") return 24576;
  return -1;
}

QString parseGeminiResponse(const QByteArray& payload)
{
  const auto document = QJsonDocument::fromJson(payload);
  const auto root = document.object();
  if (root.contains("error")) {
    throw NetworkException(QNetworkReply::ProtocolFailure,
                           extractErrorMessage(payload, QStringLiteral("Gemini request failed.")));
  }

  QStringList chunks;
  const auto candidates = root.value("candidates").toArray();
  for (const auto& candidateValue : candidates) {
    const auto candidate = candidateValue.toObject();
    const auto content = candidate.value("content").toObject();
    const auto parts = content.value("parts").toArray();
    for (const auto& partValue : parts) {
      const auto part = partValue.toObject();
      if (part.value("thought").toBool()) continue;
      const QString text = part.value("text").toString();
      if (!text.isEmpty()) chunks << text;
    }
  }

  if (chunks.isEmpty()) {
    throw NetworkException(QNetworkReply::ProtocolFailure, explainGeminiEmptyResponse(root));
  }

  return chunks.join("\n").trimmed();
}

QString openAIReasoningEffort(const QString& reasoningMode)
{
  if (reasoningMode == "off") return "none";
  if (reasoningMode == "low") return "low";
  if (reasoningMode == "medium") return "medium";
  if (reasoningMode == "high") return "high";
  return {};
}

QString runOpenAIRequest(const AIGenerationRequest& request)
{
  QJsonObject root;
  root.insert("model", request.model.trimmed());
  root.insert("store", false);
  root.insert("max_output_tokens", MAX_OUTPUT_TOKENS);
  root.insert("instructions", buildSystemPrompt());

  QJsonArray input;
  QJsonObject message;
  message.insert("role", "user");

  QJsonArray content;
  QJsonObject textPart;
  textPart.insert("type", "input_text");
  textPart.insert("text", buildUserPrompt(request));
  content.append(textPart);
  message.insert("content", content);
  input.append(message);
  root.insert("input", input);

  const QString effort = openAIReasoningEffort(request.reasoningMode.trimmed());
  if (!effort.isEmpty()) {
    QJsonObject reasoning;
    reasoning.insert("effort", effort);
    root.insert("reasoning", reasoning);
  }

  auto networkRequest = NetworkRequest<QString>(QUrl(QString::fromLatin1(OPENAI_ENDPOINT)), {200},
                                                NETWORK_TIMEOUT_SECONDS);
  return networkRequest.execute(
    [&](QNetworkRequest& network) {
      network.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
      network.setRawHeader("Authorization",
                           QStringLiteral("Bearer %1").arg(request.apiKey).toUtf8());
    },
    [&](QNetworkAccessManager& manager, QNetworkRequest& network) {
      return manager.post(network, QJsonDocument(root).toJson(QJsonDocument::Compact));
    },
    [](QNetworkReply *reply) { return parseOpenAIResponse(reply->readAll()); },
    [](QNetworkReply *reply) -> QString {
      throw NetworkException(reply->error(),
                             extractErrorMessage(reply->readAll(), reply->errorString()));
    });
}

QString runGeminiRequest(const AIGenerationRequest& request)
{
  QJsonObject root;

  QJsonObject systemInstruction;
  QJsonArray systemParts;
  QJsonObject systemPart;
  systemPart.insert("text", buildSystemPrompt());
  systemParts.append(systemPart);
  systemInstruction.insert("parts", systemParts);
  root.insert("system_instruction", systemInstruction);

  QJsonArray contents;
  QJsonObject userContent;
  userContent.insert("role", "user");
  QJsonArray userParts;
  QJsonObject userPart;
  userPart.insert("text", buildUserPrompt(request));
  userParts.append(userPart);
  userContent.insert("parts", userParts);
  contents.append(userContent);
  root.insert("contents", contents);

  QJsonObject generationConfig;
  generationConfig.insert("maxOutputTokens", MAX_OUTPUT_TOKENS);
  generationConfig.insert("responseMimeType", "text/plain");

  const QString reasoningMode = request.reasoningMode.trimmed();
  if (!reasoningMode.isEmpty() && reasoningMode != "dynamic") {
    QJsonObject thinkingConfig;
    if (isGemini3Model(request.model)) {
      const QString thinkingLevel = geminiThinkingLevel(reasoningMode);
      if (!thinkingLevel.isEmpty()) {
        thinkingConfig.insert("thinkingLevel", thinkingLevel);
      }
    } else {
      const int thinkingBudget = geminiThinkingBudget(reasoningMode);
      if (thinkingBudget >= 0) {
        thinkingConfig.insert("thinkingBudget", thinkingBudget);
      }
    }
    if (!thinkingConfig.isEmpty()) {
      generationConfig.insert("thinkingConfig", thinkingConfig);
    }
  }

  root.insert("generationConfig", generationConfig);

  const auto endpoint =
    QUrl(QString::fromLatin1(GEMINI_ENDPOINT).arg(request.model.trimmed()));
  auto networkRequest = NetworkRequest<QString>(endpoint, {200}, NETWORK_TIMEOUT_SECONDS);
  return networkRequest.execute(
    [&](QNetworkRequest& network) {
      network.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
      if (!request.bearerAccessToken.trimmed().isEmpty()) {
        network.setRawHeader("Authorization",
                             QStringLiteral("Bearer %1").arg(request.bearerAccessToken.trimmed()).toUtf8());
        if (!request.googleProjectId.trimmed().isEmpty()) {
          network.setRawHeader("x-goog-user-project", request.googleProjectId.trimmed().toUtf8());
        }
      } else {
        network.setRawHeader("x-goog-api-key", request.apiKey.toUtf8());
      }
    },
    [&](QNetworkAccessManager& manager, QNetworkRequest& network) {
      return manager.post(network, QJsonDocument(root).toJson(QJsonDocument::Compact));
    },
    [](QNetworkReply *reply) { return parseGeminiResponse(reply->readAll()); },
    [](QNetworkReply *reply) -> QString {
      throw NetworkException(reply->error(),
                             extractErrorMessage(reply->readAll(), reply->errorString()));
    });
}

bool validateGeminiReasoning(const AIGenerationRequest& request, QString *error)
{
  if (request.reasoningMode != "off" || !isGeminiProModel(request.model)) {
    return true;
  }

  if (error != nullptr) {
    *error = isGemini3Model(request.model)
               ? QStringLiteral("Gemini 3 Pro models do not support disabling thinking.")
               : QStringLiteral("Gemini 2.5 Pro models do not support disabling thinking.");
  }
  return false;
}

}  // namespace

AIGenerationResult AICodeGenerator::generate(const AIGenerationRequest& request)
{
  const bool hasGeminiOAuth =
    request.provider == AIProvider::Gemini && !request.bearerAccessToken.trimmed().isEmpty();

  if (request.apiKey.trimmed().isEmpty() && !hasGeminiOAuth) {
    return errorResult(QStringLiteral("%1 API key is empty.").arg(providerName(request.provider)));
  }
  if (request.model.trimmed().isEmpty()) {
    return errorResult(QStringLiteral("Select or enter a model name."));
  }
  if (request.prompt.trimmed().isEmpty()) {
    return errorResult(QStringLiteral("Enter a prompt before generating code."));
  }

  if (request.provider == AIProvider::Gemini) {
    QString geminiError;
    if (!validateGeminiReasoning(request, &geminiError)) {
      return errorResult(geminiError);
    }
  }

  try {
    const QString rawText = request.provider == AIProvider::OpenAI ? runOpenAIRequest(request)
                                                                   : runGeminiRequest(request);
    const QString code = normalizeCodeResponse(rawText);
    if (code.trimmed().isEmpty()) {
      return errorResult(QStringLiteral("%1 returned an empty response.")
                           .arg(providerName(request.provider)));
    }

    AIGenerationResult result;
    result.success = true;
    result.rawText = rawText;
    result.code = code;
    return result;
  } catch (const NetworkException& error) {
    return errorResult(QString::fromStdString(error.getErrorMessage()));
  } catch (const std::exception& error) {
    return errorResult(QString::fromStdString(error.what()));
  } catch (...) {
    return errorResult(QStringLiteral("The AI request failed for an unknown reason."));
  }
}
