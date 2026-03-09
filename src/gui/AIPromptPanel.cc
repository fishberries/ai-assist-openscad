#include "gui/AIPromptPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include "gui/GoogleOAuthManager.h"
#include "gui/MainWindow.h"
#include "gui/QSettingsCached.h"
#include "gui/SecretStorage.h"

namespace {

constexpr auto SETTING_PROVIDER = "ai/provider";
constexpr auto SETTING_OPENAI_KEY = "ai/openaiApiKey";
constexpr auto SETTING_GEMINI_KEY = "ai/geminiApiKey";
constexpr auto SETTING_GEMINI_AUTH_MODE = "ai/geminiAuthMode";
constexpr auto SETTING_GEMINI_OAUTH_CLIENT_FILE = "ai/geminiOAuthClientFile";
constexpr auto SETTING_GEMINI_PROJECT_ID = "ai/geminiProjectId";
constexpr auto SETTING_GEMINI_OAUTH_TOKEN_JSON = "ai/geminiOAuthTokenJson";
constexpr auto SETTING_REASONING = "ai/reasoningMode";
constexpr auto SETTING_PERMISSION = "ai/permissionMode";
constexpr auto SETTING_INCLUDE_DOCUMENT = "ai/includeDocument";
constexpr auto SETTING_INCLUDE_SELECTION = "ai/includeSelection";
constexpr auto SETTING_MODEL_OPENAI = "ai/model/openai";
constexpr auto SETTING_MODEL_GEMINI = "ai/model/gemini";
constexpr auto GOOGLE_AI_STUDIO_URL = "https://aistudio.google.com/";
constexpr auto GOOGLE_AI_STUDIO_API_KEYS_URL = "https://aistudio.google.com/app/apikey";

QString defaultModel(AIProvider provider)
{
  return provider == AIProvider::OpenAI ? QStringLiteral("gpt-5-codex")
                                        : QStringLiteral("gemini-2.5-pro");
}

QStringList modelListForProvider(AIProvider provider)
{
  if (provider == AIProvider::OpenAI) {
    return {
      QStringLiteral("gpt-5-codex"),
      QStringLiteral("gpt-5.4"),
      QStringLiteral("gpt-5.4-pro"),
      QStringLiteral("gpt-5.3-codex"),
      QStringLiteral("gpt-5.2-codex"),
      QStringLiteral("gpt-5.1"),
      QStringLiteral("gpt-5.1-codex"),
    };
  }

  return {
    QStringLiteral("gemini-2.5-pro"),
    QStringLiteral("gemini-2.5-flash"),
    QStringLiteral("gemini-2.5-flash-lite"),
    QStringLiteral("gemini-3-pro-preview"),
    QStringLiteral("gemini-3-flash-preview"),
  };
}

QString providerSettingValue(AIProvider provider)
{
  return provider == AIProvider::OpenAI ? QStringLiteral("openai") : QStringLiteral("gemini");
}

AIProvider providerFromSetting(const QString& value)
{
  return value.compare("gemini", Qt::CaseInsensitive) == 0 ? AIProvider::Gemini : AIProvider::OpenAI;
}

QString providerLabel(AIProvider provider)
{
  return provider == AIProvider::OpenAI ? QStringLiteral("OpenAI") : QStringLiteral("Gemini");
}

QString firstEnvironmentValue(std::initializer_list<const char *> variableNames)
{
  for (const auto *variableName : variableNames) {
    const QString value = qEnvironmentVariable(variableName).trimmed();
    if (!value.isEmpty()) return value;
  }
  return {};
}

QWidget *createLabeledRow(const QString& labelText, QWidget *field, QWidget *parent,
                          QWidget *extraWidget = nullptr)
{
  auto *rowWidget = new QWidget(parent);
  auto *layout = new QHBoxLayout(rowWidget);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(6);

  auto *label = new QLabel(labelText, rowWidget);
  label->setMinimumWidth(130);
  layout->addWidget(label);
  layout->addWidget(field, 1);
  if (extraWidget != nullptr) {
    layout->addWidget(extraWidget);
  }

  return rowWidget;
}

}  // namespace

AIPromptPanel::AIPromptPanel(MainWindow *mainWindow, QWidget *parent)
  : QWidget(parent), mainWindow(mainWindow)
{
  setObjectName("aiPromptPanel");
  setMinimumWidth(280);
  setMaximumWidth(460);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

  const auto fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);

  auto *outerLayout = new QVBoxLayout(this);
  outerLayout->setContentsMargins(0, 0, 0, 0);
  outerLayout->setSpacing(0);

  auto *scrollArea = new QScrollArea(this);
  scrollArea->setWidgetResizable(true);
  scrollArea->setFrameShape(QFrame::NoFrame);
  scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  outerLayout->addWidget(scrollArea);

  auto *contentWidget = new QWidget(scrollArea);
  scrollArea->setWidget(contentWidget);

  auto *mainLayout = new QVBoxLayout(contentWidget);
  mainLayout->setContentsMargins(8, 8, 8, 8);
  mainLayout->setSpacing(8);

  auto *titleLabel = new QLabel(tr("AI Code Generation"), contentWidget);
  QFont titleFont = titleLabel->font();
  titleFont.setBold(true);
  titleFont.setPointSize(titleFont.pointSize() + 1);
  titleLabel->setFont(titleFont);
  mainLayout->addWidget(titleLabel);

  auto *connectionBox = new QGroupBox(tr("Session"), contentWidget);
  auto *connectionLayout = new QVBoxLayout(connectionBox);
  connectionLayout->setContentsMargins(8, 8, 8, 8);
  connectionLayout->setSpacing(6);

  providerCombo = new QComboBox(connectionBox);
  providerCombo->addItem(tr("OpenAI"), providerSettingValue(AIProvider::OpenAI));
  providerCombo->addItem(tr("Gemini"), providerSettingValue(AIProvider::Gemini));
  connectionLayout->addWidget(createLabeledRow(tr("Provider"), providerCombo, connectionBox));

  modelCombo = new QComboBox(connectionBox);
  modelCombo->setEditable(true);
  modelCombo->setInsertPolicy(QComboBox::NoInsert);
  connectionLayout->addWidget(createLabeledRow(tr("Available Models"), modelCombo, connectionBox));

  reasoningCombo = new QComboBox(connectionBox);
  reasoningCombo->addItem(tr("Dynamic"), "dynamic");
  reasoningCombo->addItem(tr("Low"), "low");
  reasoningCombo->addItem(tr("Medium"), "medium");
  reasoningCombo->addItem(tr("High"), "high");
  reasoningCombo->addItem(tr("Off"), "off");
  connectionLayout->addWidget(createLabeledRow(tr("Reasoning"), reasoningCombo, connectionBox));

  providerInstructionsLabel = new QLabel(connectionBox);
  providerInstructionsLabel->setWordWrap(true);
  connectionLayout->addWidget(providerInstructionsLabel);

  auto *geminiQuickActions = new QWidget(connectionBox);
  auto *geminiQuickActionsLayout = new QHBoxLayout(geminiQuickActions);
  geminiQuickActionsLayout->setContentsMargins(0, 0, 0, 0);
  geminiQuickActionsLayout->setSpacing(6);
  auto *openGeminiStudioButton = new QPushButton(tr("Open Google AI Studio"), geminiQuickActions);
  auto *openGeminiApiKeysButton = new QPushButton(tr("Open API Keys"), geminiQuickActions);
  auto *importGeminiEnvButton = new QPushButton(tr("Import Env Key"), geminiQuickActions);
  geminiQuickActionsLayout->addWidget(openGeminiStudioButton);
  geminiQuickActionsLayout->addWidget(openGeminiApiKeysButton);
  geminiQuickActionsLayout->addWidget(importGeminiEnvButton);
  geminiQuickActionsRowWidget = geminiQuickActions;
  connectionLayout->addWidget(geminiQuickActionsRowWidget);

  permissionCombo = new QComboBox(connectionBox);
  permissionCombo->addItem(tr("Suggest Only"), "suggest");
  permissionCombo->addItem(tr("Insert at Cursor"), "insert");
  permissionCombo->addItem(tr("Replace Selection"), "replace-selection");
  permissionCombo->addItem(tr("Replace Document"), "replace-document");
  permissionCombo->setToolTip(tr("Controls what Apply is allowed to change in the editor."));
  connectionLayout->addWidget(createLabeledRow(tr("Permission Policy"), permissionCombo, connectionBox));

  reasoningHintLabel = new QLabel(connectionBox);
  reasoningHintLabel->setWordWrap(true);
  connectionLayout->addWidget(reasoningHintLabel);
  mainLayout->addWidget(connectionBox);

  auto *credentialsBox = new QGroupBox(tr("Credentials"), contentWidget);
  auto *credentialsLayout = new QVBoxLayout(credentialsBox);
  credentialsLayout->setContentsMargins(8, 8, 8, 8);
  credentialsLayout->setSpacing(6);

  openAIKeyEdit = new QLineEdit(credentialsBox);
  openAIKeyEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
  openAIKeyEdit->setPlaceholderText(tr("sk-... or OPENAI_API_KEY"));
  openAIKeyRowWidget = createLabeledRow(tr("OpenAI API Key"), openAIKeyEdit, credentialsBox);
  credentialsLayout->addWidget(openAIKeyRowWidget);

  geminiAuthModeCombo = new QComboBox(credentialsBox);
  geminiAuthModeCombo->addItem(tr("API Key (Recommended)"), "api-key");
  geminiAuthModeCombo->addItem(tr("Advanced Google OAuth"), "google-oauth");
  geminiAuthModeRowWidget = createLabeledRow(tr("Gemini Access"), geminiAuthModeCombo, credentialsBox);
  credentialsLayout->addWidget(geminiAuthModeRowWidget);

  geminiKeyEdit = new QLineEdit(credentialsBox);
  geminiKeyEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
  geminiKeyEdit->setPlaceholderText(tr("AIza... or GEMINI_API_KEY / GOOGLE_API_KEY"));
  geminiApiKeyRowWidget = createLabeledRow(tr("Gemini API Key"), geminiKeyEdit, credentialsBox);
  credentialsLayout->addWidget(geminiApiKeyRowWidget);

  geminiOAuthClientFileEdit = new QLineEdit(credentialsBox);
  geminiOAuthClientFileEdit->setPlaceholderText(tr("client_secret_*.json"));
  geminiOAuthBrowseButton = new QPushButton(tr("Browse"), credentialsBox);
  geminiOAuthFileRowWidget =
    createLabeledRow(tr("OAuth Client JSON"), geminiOAuthClientFileEdit, credentialsBox,
                     geminiOAuthBrowseButton);
  credentialsLayout->addWidget(geminiOAuthFileRowWidget);

  geminiProjectIdEdit = new QLineEdit(credentialsBox);
  geminiProjectIdEdit->setPlaceholderText(tr("Google Cloud project id"));
  geminiProjectRowWidget = createLabeledRow(tr("Billing Project"), geminiProjectIdEdit, credentialsBox);
  credentialsLayout->addWidget(geminiProjectRowWidget);

  auto *geminiLoginButtons = new QWidget(credentialsBox);
  auto *geminiLoginLayout = new QHBoxLayout(geminiLoginButtons);
  geminiLoginLayout->setContentsMargins(0, 0, 0, 0);
  geminiLoginLayout->setSpacing(6);
  auto *geminiLoginCaption = new QLabel(tr("Google Session"), geminiLoginButtons);
  geminiLoginCaption->setMinimumWidth(130);
  geminiSignInButton = new QPushButton(tr("Sign In"), geminiLoginButtons);
  geminiSignOutButton = new QPushButton(tr("Sign Out"), geminiLoginButtons);
  geminiLoginStatusLabel = new QLabel(geminiLoginButtons);
  geminiLoginStatusLabel->setWordWrap(true);
  geminiLoginLayout->addWidget(geminiLoginCaption);
  geminiLoginLayout->addWidget(geminiSignInButton);
  geminiLoginLayout->addWidget(geminiSignOutButton);
  geminiLoginLayout->addWidget(geminiLoginStatusLabel, 1);
  geminiLoginButtonsRowWidget = geminiLoginButtons;
  credentialsLayout->addWidget(geminiLoginButtonsRowWidget);

  showKeysCheck = new QCheckBox(tr("Show API keys"), credentialsBox);
  credentialsLayout->addWidget(showKeysCheck);

  auto *credentialsNote = new QLabel(
    tr("Keys and Google refresh tokens are stored in Windows Credential Manager on this machine. "
       "Gemini's easiest supported path here is a Google AI Studio API key. "
       "Advanced Google OAuth still requires a Desktop OAuth client JSON and a billed Google Cloud project."),
    credentialsBox);
  credentialsNote->setWordWrap(true);
  credentialsLayout->addWidget(credentialsNote);
  mainLayout->addWidget(credentialsBox);

  auto *contextBox = new QGroupBox(tr("Context"), contentWidget);
  auto *contextLayout = new QVBoxLayout(contextBox);
  includeDocumentCheck = new QCheckBox(tr("Include the current document"), contextBox);
  includeSelectionCheck = new QCheckBox(tr("Include the current selection"), contextBox);
  contextLayout->addWidget(includeDocumentCheck);
  contextLayout->addWidget(includeSelectionCheck);
  mainLayout->addWidget(contextBox);

  auto *promptBox = new QGroupBox(tr("Prompt"), contentWidget);
  auto *promptLayout = new QVBoxLayout(promptBox);
  promptEdit = new QPlainTextEdit(promptBox);
  promptEdit->setMinimumHeight(140);
  promptEdit->setPlaceholderText(tr("Describe the OpenSCAD code you want to generate or modify."));
  promptEdit->setFont(fixedFont);
  promptLayout->addWidget(promptEdit);
  mainLayout->addWidget(promptBox, 1);

  auto *resultBox = new QGroupBox(tr("Generated Code"), contentWidget);
  auto *resultLayout = new QVBoxLayout(resultBox);
  responseEdit = new QPlainTextEdit(resultBox);
  responseEdit->setReadOnly(true);
  responseEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
  responseEdit->setMinimumHeight(220);
  responseEdit->setFont(fixedFont);
  resultLayout->addWidget(responseEdit, 1);

  auto *buttonLayout = new QHBoxLayout();
  generateButton = new QPushButton(tr("Generate"), resultBox);
  applyButton = new QPushButton(tr("Apply"), resultBox);
  copyButton = new QPushButton(tr("Copy"), resultBox);
  buttonLayout->addWidget(generateButton);
  buttonLayout->addWidget(applyButton);
  buttonLayout->addWidget(copyButton);
  resultLayout->addLayout(buttonLayout);
  mainLayout->addWidget(resultBox, 1);

  statusLabel = new QLabel(
    tr("Choose a provider, configure its access method, then write a prompt and click Generate."),
    contentWidget);
  statusLabel->setWordWrap(true);
  mainLayout->addWidget(statusLabel);

  const auto persistOpenAISecret = [this]() {
    SecretStorage::saveSecret(QString::fromLatin1(SETTING_OPENAI_KEY), openAIKeyEdit->text().trimmed());
  };
  const auto persistGeminiSecret = [this]() {
    SecretStorage::saveSecret(QString::fromLatin1(SETTING_GEMINI_KEY), geminiKeyEdit->text().trimmed());
  };

  connect(&generationWatcher, &QFutureWatcher<AIGenerationResult>::finished, this,
          &AIPromptPanel::onGenerationFinished);
  connect(providerCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AIPromptPanel::onProviderChanged);
  connect(providerCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AIPromptPanel::saveCurrentProvider);
  connect(openGeminiStudioButton, &QPushButton::clicked, this, [this]() {
    providerCombo->setCurrentIndex(providerCombo->findData(providerSettingValue(AIProvider::Gemini)));
    geminiAuthModeCombo->setCurrentIndex(geminiAuthModeCombo->findData("api-key"));
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(GOOGLE_AI_STUDIO_URL)));
    geminiKeyEdit->setFocus();
  });
  connect(openGeminiApiKeysButton, &QPushButton::clicked, this, [this]() {
    providerCombo->setCurrentIndex(providerCombo->findData(providerSettingValue(AIProvider::Gemini)));
    geminiAuthModeCombo->setCurrentIndex(geminiAuthModeCombo->findData("api-key"));
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(GOOGLE_AI_STUDIO_API_KEYS_URL)));
    geminiKeyEdit->setFocus();
  });
  connect(importGeminiEnvButton, &QPushButton::clicked, this, [this]() {
    providerCombo->setCurrentIndex(providerCombo->findData(providerSettingValue(AIProvider::Gemini)));
    geminiAuthModeCombo->setCurrentIndex(geminiAuthModeCombo->findData("api-key"));
    const QString envKey = firstEnvironmentValue({"GEMINI_API_KEY", "GOOGLE_API_KEY"});
    if (envKey.isEmpty()) {
      setStatus(tr("Set GEMINI_API_KEY or GOOGLE_API_KEY, or create a key in Google AI Studio."));
      geminiKeyEdit->setFocus();
      return;
    }

    geminiKeyEdit->setText(envKey);
    setStatus(tr("Loaded Gemini API key from the environment."));
    geminiKeyEdit->setFocus();
  });
  connect(modelCombo, &QComboBox::currentTextChanged, this, &AIPromptPanel::saveCurrentProviderModel);
  connect(modelCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
    updateReasoningHint();
  });
  connect(reasoningCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AIPromptPanel::saveCurrentReasoning);
  connect(reasoningCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AIPromptPanel::updateReasoningHint);
  connect(permissionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AIPromptPanel::saveCurrentPermission);
  connect(permissionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AIPromptPanel::refreshContextState);
  connect(includeDocumentCheck, &QCheckBox::toggled, this, &AIPromptPanel::saveContextOptions);
  connect(includeSelectionCheck, &QCheckBox::toggled, this, &AIPromptPanel::saveContextOptions);
  connect(geminiAuthModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AIPromptPanel::onGeminiAuthModeChanged);
  connect(geminiAuthModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AIPromptPanel::saveGeminiAuthMode);
  connect(geminiAuthModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AIPromptPanel::updateProviderInstructions);
  connect(geminiOAuthBrowseButton, &QPushButton::clicked, this,
          &AIPromptPanel::onGeminiOAuthBrowseClicked);
  connect(geminiSignInButton, &QPushButton::clicked, this, &AIPromptPanel::onGeminiSignInClicked);
  connect(geminiSignOutButton, &QPushButton::clicked, this, &AIPromptPanel::onGeminiSignOutClicked);
  connect(openAIKeyEdit, &QLineEdit::textChanged, this, [persistOpenAISecret](const QString&) {
    persistOpenAISecret();
  });
  connect(openAIKeyEdit, &QLineEdit::editingFinished, this, persistOpenAISecret);
  connect(geminiKeyEdit, &QLineEdit::textChanged, this, [persistGeminiSecret](const QString&) {
    persistGeminiSecret();
  });
  connect(geminiKeyEdit, &QLineEdit::editingFinished, this, persistGeminiSecret);
  connect(geminiOAuthClientFileEdit, &QLineEdit::textChanged, this,
          &AIPromptPanel::saveGeminiOAuthSettings);
  connect(geminiProjectIdEdit, &QLineEdit::textChanged, this,
          &AIPromptPanel::saveGeminiOAuthSettings);
  connect(showKeysCheck, &QCheckBox::toggled, this, &AIPromptPanel::onShowKeysToggled);
  connect(generateButton, &QPushButton::clicked, this, &AIPromptPanel::onGenerateClicked);
  connect(applyButton, &QPushButton::clicked, this, &AIPromptPanel::onApplyClicked);
  connect(copyButton, &QPushButton::clicked, this, &AIPromptPanel::onCopyClicked);

  loadSettings();
  refreshContextState();
}

void AIPromptPanel::refreshContextState()
{
  updateGeminiLoginStatus();
  updateApplyButton();
}

void AIPromptPanel::loadSettings()
{
  const QSettingsCached settings;

  const QString openAIKey = SecretStorage::loadSecret(QString::fromLatin1(SETTING_OPENAI_KEY));
  openAIKeyEdit->setText(openAIKey);
  if (!openAIKey.isEmpty()) {
    SecretStorage::saveSecret(QString::fromLatin1(SETTING_OPENAI_KEY), openAIKey);
  }

  const QString geminiKey = SecretStorage::loadSecret(QString::fromLatin1(SETTING_GEMINI_KEY));
  geminiKeyEdit->setText(geminiKey);
  if (!geminiKey.isEmpty()) {
    SecretStorage::saveSecret(QString::fromLatin1(SETTING_GEMINI_KEY), geminiKey);
  }

  geminiOAuthClientFileEdit->setText(settings.value(SETTING_GEMINI_OAUTH_CLIENT_FILE).toString());
  geminiProjectIdEdit->setText(settings.value(SETTING_GEMINI_PROJECT_ID).toString());
  includeDocumentCheck->setChecked(settings.value(SETTING_INCLUDE_DOCUMENT, true).toBool());
  includeSelectionCheck->setChecked(settings.value(SETTING_INCLUDE_SELECTION, true).toBool());

  {
    const QSignalBlocker blocker(providerCombo);
    const AIProvider provider = providerFromSetting(settings.value(SETTING_PROVIDER, "openai").toString());
    providerCombo->setCurrentIndex(provider == AIProvider::OpenAI ? 0 : 1);
  }

  {
    const QSignalBlocker blocker(geminiAuthModeCombo);
    const QString authMode = settings.value(SETTING_GEMINI_AUTH_MODE, "api-key").toString();
    const int index = geminiAuthModeCombo->findData(authMode);
    geminiAuthModeCombo->setCurrentIndex(index >= 0 ? index : 0);
  }

  {
    const QSignalBlocker blocker(reasoningCombo);
    const QString reasoning = settings.value(SETTING_REASONING, "dynamic").toString();
    const int index = reasoningCombo->findData(reasoning);
    reasoningCombo->setCurrentIndex(index >= 0 ? index : 0);
  }

  {
    const QSignalBlocker blocker(permissionCombo);
    const QString permission = settings.value(SETTING_PERMISSION, "suggest").toString();
    const int index = permissionCombo->findData(permission);
    permissionCombo->setCurrentIndex(index >= 0 ? index : 0);
  }

  populateModels();
  updateProviderInstructions();
  updateReasoningHint();
  updateCredentialVisibility();
}

void AIPromptPanel::populateModels()
{
  const AIProvider provider = currentProvider();
  const QSettingsCached settings;
  QString savedModel =
    settings.value(settingKeyForProviderModel(provider), defaultModel(provider)).toString();
  if (savedModel.trimmed().isEmpty()) {
    savedModel = defaultModel(provider);
  }

  const QSignalBlocker blocker(modelCombo);
  modelCombo->clear();
  modelCombo->addItems(modelListForProvider(provider));
  if (modelCombo->findText(savedModel) < 0) {
    modelCombo->addItem(savedModel);
  }
  modelCombo->setCurrentText(savedModel);
}

void AIPromptPanel::setStatus(const QString& status)
{
  statusLabel->setText(status);
}

void AIPromptPanel::setBusy(bool busy)
{
  generateButton->setEnabled(!busy);
  providerCombo->setEnabled(!busy);
  modelCombo->setEnabled(!busy);
  reasoningCombo->setEnabled(!busy);
  permissionCombo->setEnabled(!busy);
  geminiAuthModeCombo->setEnabled(!busy);
  openAIKeyEdit->setEnabled(!busy);
  geminiKeyEdit->setEnabled(!busy);
  geminiOAuthClientFileEdit->setEnabled(!busy);
  geminiProjectIdEdit->setEnabled(!busy);
  geminiOAuthBrowseButton->setEnabled(!busy);
  geminiSignInButton->setEnabled(!busy);
  geminiSignOutButton->setEnabled(!busy);
  showKeysCheck->setEnabled(!busy);
  includeDocumentCheck->setEnabled(!busy);
  includeSelectionCheck->setEnabled(!busy);
  promptEdit->setEnabled(!busy);
  copyButton->setEnabled(!busy && !lastGeneratedCode.isEmpty());
  updateApplyButton();
}

void AIPromptPanel::updateApplyButton()
{
  const QString permission = currentPermissionMode();
  const bool hasCode = !lastGeneratedCode.trimmed().isEmpty();
  auto *editor = mainWindow != nullptr ? mainWindow->activeEditor : nullptr;

  if (!hasCode) {
    applyButton->setText(tr("Apply"));
    applyButton->setEnabled(false);
    copyButton->setEnabled(false);
    return;
  }

  copyButton->setEnabled(generateButton->isEnabled());

  if (permission == "suggest") {
    applyButton->setText(tr("Apply Disabled"));
    applyButton->setEnabled(false);
    return;
  }

  if (permission == "insert") {
    applyButton->setText(tr("Insert in Editor"));
  } else if (permission == "replace-selection") {
    applyButton->setText(tr("Replace Selection"));
  } else {
    applyButton->setText(tr("Replace Document"));
  }

  applyButton->setEnabled(generateButton->isEnabled() && editor != nullptr);
}

void AIPromptPanel::updateCredentialVisibility()
{
  const bool isOpenAI = currentProvider() == AIProvider::OpenAI;
  const bool isGemini = currentProvider() == AIProvider::Gemini;
  const bool geminiOAuth = isGemini && usesGeminiOAuth();

  openAIKeyRowWidget->setVisible(isOpenAI);
  geminiQuickActionsRowWidget->setVisible(isGemini);
  geminiAuthModeRowWidget->setVisible(isGemini);
  geminiApiKeyRowWidget->setVisible(isGemini && !geminiOAuth);
  geminiOAuthFileRowWidget->setVisible(geminiOAuth);
  geminiProjectRowWidget->setVisible(geminiOAuth);
  geminiLoginButtonsRowWidget->setVisible(geminiOAuth);
  showKeysCheck->setVisible(isOpenAI || (isGemini && !geminiOAuth));
}

void AIPromptPanel::updateGeminiLoginStatus()
{
  if (!usesGeminiOAuth()) {
    geminiLoginStatusLabel->setText(QString());
    return;
  }

  const QString clientFile = geminiOAuthClientFileEdit->text().trimmed();
  if (clientFile.isEmpty()) {
    geminiLoginStatusLabel->setText(tr("Choose a Desktop App OAuth client JSON."));
    return;
  }

  const auto configResult = GoogleOAuthManager::loadClientConfig(clientFile);
  if (!configResult.success) {
    geminiLoginStatusLabel->setText(configResult.error);
    return;
  }

  if (geminiProjectIdEdit->text().trimmed().isEmpty() && !configResult.clientConfig.projectId.isEmpty()) {
    geminiProjectIdEdit->setText(configResult.clientConfig.projectId);
  }

  const auto token = GoogleOAuthManager::loadToken(
    SecretStorage::loadSecret(QString::fromLatin1(SETTING_GEMINI_OAUTH_TOKEN_JSON)));
  if (token.hasUsableAccessToken()) {
    geminiLoginStatusLabel->setText(tr("Signed in. Access token is ready."));
  } else if (!token.refreshToken.isEmpty()) {
    geminiLoginStatusLabel->setText(tr("Signed in. Access token will refresh on the next request."));
  } else {
    geminiLoginStatusLabel->setText(tr("Not signed in."));
  }
}

AIProvider AIPromptPanel::currentProvider() const
{
  return providerFromSetting(providerCombo->currentData().toString());
}

QString AIPromptPanel::currentApiKey() const
{
  if (currentProvider() == AIProvider::OpenAI) {
    const QString explicitKey = openAIKeyEdit->text().trimmed();
    return explicitKey.isEmpty() ? firstEnvironmentValue({"OPENAI_API_KEY"}) : explicitKey;
  }
  if (usesGeminiOAuth()) return QString();

  const QString explicitKey = geminiKeyEdit->text().trimmed();
  return explicitKey.isEmpty() ? firstEnvironmentValue({"GEMINI_API_KEY", "GOOGLE_API_KEY"})
                               : explicitKey;
}

QString AIPromptPanel::currentModel() const
{
  return modelCombo->currentText().trimmed();
}

QString AIPromptPanel::currentReasoningMode() const
{
  return reasoningCombo->currentData().toString();
}

QString AIPromptPanel::currentPermissionMode() const
{
  return permissionCombo->currentData().toString();
}

bool AIPromptPanel::usesGeminiOAuth() const
{
  return geminiAuthModeCombo->currentData().toString() == "google-oauth";
}

QString AIPromptPanel::settingKeyForProviderModel(AIProvider provider) const
{
  return provider == AIProvider::OpenAI ? QString::fromLatin1(SETTING_MODEL_OPENAI)
                                        : QString::fromLatin1(SETTING_MODEL_GEMINI);
}

AIGenerationRequest AIPromptPanel::buildRequest(QString *errorMessage)
{
  AIGenerationRequest request;
  request.provider = currentProvider();
  request.apiKey = currentApiKey();
  request.model = currentModel();
  request.reasoningMode = currentReasoningMode();
  request.permissionMode = currentPermissionMode();
  request.prompt = promptEdit->toPlainText();
  request.includeDocument = includeDocumentCheck->isChecked();
  request.includeSelection = includeSelectionCheck->isChecked();

  if (request.provider == AIProvider::Gemini && usesGeminiOAuth()) {
    const QString clientFile = geminiOAuthClientFileEdit->text().trimmed();
    if (clientFile.isEmpty()) {
      if (errorMessage != nullptr) {
        *errorMessage = tr("Choose a Google OAuth client JSON before generating code.");
      }
      return request;
    }

    const auto refreshResult = GoogleOAuthManager::refreshGeminiToken(
      clientFile, SecretStorage::loadSecret(QString::fromLatin1(SETTING_GEMINI_OAUTH_TOKEN_JSON)));
    if (!refreshResult.success) {
      if (errorMessage != nullptr) {
        *errorMessage = refreshResult.error;
      }
      return request;
    }

    SecretStorage::saveSecret(QString::fromLatin1(SETTING_GEMINI_OAUTH_TOKEN_JSON),
                              GoogleOAuthManager::serializeToken(refreshResult.token));

    QString projectId = geminiProjectIdEdit->text().trimmed();
    if (projectId.isEmpty()) {
      projectId = refreshResult.clientConfig.projectId.trimmed();
      if (!projectId.isEmpty()) {
        geminiProjectIdEdit->setText(projectId);
      }
    }

    if (projectId.isEmpty()) {
      if (errorMessage != nullptr) {
        *errorMessage = tr("Enter the Google Cloud billing project id for Gemini OAuth access.");
      }
      return request;
    }

    request.bearerAccessToken = refreshResult.token.accessToken;
    request.googleProjectId = projectId;
  }

  auto *editor = mainWindow != nullptr ? mainWindow->activeEditor : nullptr;
  if (editor != nullptr) {
    request.filePath = editor->filepath;
    if (request.includeDocument) request.documentText = editor->toPlainText();
    if (request.includeSelection) request.selectedText = editor->selectedText();
  }
  return request;
}

void AIPromptPanel::onProviderChanged()
{
  populateModels();
  updateProviderInstructions();
  updateReasoningHint();
  updateCredentialVisibility();
  refreshContextState();
}

void AIPromptPanel::onGenerationFinished()
{
  const auto result = generationWatcher.result();
  setBusy(false);

  if (!result.success) {
    lastGeneratedCode.clear();
    responseEdit->clear();
    setStatus(result.error);
    refreshContextState();
    return;
  }

  lastGeneratedCode = result.code;
  responseEdit->setPlainText(result.code);
  setStatus(tr("%1 returned code for %2. Review it, then apply it using the selected permission policy.")
              .arg(providerLabel(currentProvider()), currentModel()));
  refreshContextState();
}

void AIPromptPanel::onGenerateClicked()
{
  SecretStorage::saveSecret(QString::fromLatin1(SETTING_OPENAI_KEY), openAIKeyEdit->text().trimmed());
  SecretStorage::saveSecret(QString::fromLatin1(SETTING_GEMINI_KEY), geminiKeyEdit->text().trimmed());

  QString buildError;
  const auto request = buildRequest(&buildError);
  if (!buildError.isEmpty()) {
    setStatus(buildError);
    return;
  }
  if (request.prompt.trimmed().isEmpty()) {
    setStatus(tr("Enter a prompt before generating code."));
    return;
  }
  if (request.provider == AIProvider::OpenAI && request.apiKey.isEmpty()) {
    setStatus(tr("Enter the OpenAI API key, or set OPENAI_API_KEY, before generating code."));
    return;
  }
  if (request.provider == AIProvider::Gemini && !usesGeminiOAuth() && request.apiKey.isEmpty()) {
    setStatus(tr("Enter the Gemini API key, set GEMINI_API_KEY / GOOGLE_API_KEY, or create one in Google AI Studio before generating code."));
    return;
  }
  if (request.model.isEmpty()) {
    setStatus(tr("Enter a model name before generating code."));
    return;
  }

  lastGeneratedCode.clear();
  responseEdit->clear();
  setBusy(true);
  setStatus(tr("Generating OpenSCAD code with %1 using %2...")
              .arg(providerLabel(request.provider), request.model));
  generationWatcher.setFuture(QtConcurrent::run([request] { return AICodeGenerator::generate(request); }));
}

void AIPromptPanel::onApplyClicked()
{
  auto *editor = mainWindow != nullptr ? mainWindow->activeEditor : nullptr;
  if (editor == nullptr) {
    setStatus(tr("There is no active editor to apply the generated code to."));
    return;
  }
  if (lastGeneratedCode.trimmed().isEmpty()) {
    setStatus(tr("Generate code before applying it."));
    return;
  }

  const QString permission = currentPermissionMode();
  if (permission == "suggest") {
    setStatus(tr("Permission Policy is set to Suggest Only, so editor changes are disabled."));
    return;
  }

  if (permission == "insert") {
    editor->insert(lastGeneratedCode);
    setStatus(tr("Inserted the generated code into the current editor."));
  } else if (permission == "replace-selection") {
    if (!editor->replaceSelectedText(lastGeneratedCode)) {
      setStatus(tr("Select code in the editor before using Replace Selection."));
      return;
    }
    setStatus(tr("Replaced the current selection with the generated code."));
  } else if (permission == "replace-document") {
    editor->setText(lastGeneratedCode);
    setStatus(tr("Replaced the current document with the generated code."));
  }

  editor->setFocus();
  refreshContextState();
}

void AIPromptPanel::onCopyClicked()
{
  if (lastGeneratedCode.trimmed().isEmpty()) {
    setStatus(tr("There is no generated code to copy."));
    return;
  }

  if (auto *clipboard = QApplication::clipboard()) {
    clipboard->setText(lastGeneratedCode);
    setStatus(tr("Copied the generated code to the clipboard."));
  }
}

void AIPromptPanel::onGeminiAuthModeChanged()
{
  updateProviderInstructions();
  updateCredentialVisibility();
  refreshContextState();
}

void AIPromptPanel::onGeminiOAuthBrowseClicked()
{
  const QString path = QFileDialog::getOpenFileName(this, tr("Select Google OAuth Client JSON"),
                                                    geminiOAuthClientFileEdit->text(),
                                                    tr("JSON Files (*.json)"));
  if (path.isEmpty()) return;

  geminiOAuthClientFileEdit->setText(path);
  const auto configResult = GoogleOAuthManager::loadClientConfig(path);
  if (!configResult.success) {
    setStatus(configResult.error);
    return;
  }

  if (geminiProjectIdEdit->text().trimmed().isEmpty() && !configResult.clientConfig.projectId.isEmpty()) {
    geminiProjectIdEdit->setText(configResult.clientConfig.projectId);
  }
  setStatus(tr("Loaded Google OAuth client settings for Gemini."));
  refreshContextState();
}

void AIPromptPanel::onGeminiSignInClicked()
{
  const QString clientFile = geminiOAuthClientFileEdit->text().trimmed();
  if (clientFile.isEmpty()) {
    setStatus(tr("Choose a Google OAuth client JSON before signing in."));
    return;
  }

  setStatus(tr("Opening the browser for Google login..."));
  const auto result = GoogleOAuthManager::authorizeForGemini(this, clientFile);
  if (!result.success) {
    setStatus(result.error);
    refreshContextState();
    return;
  }

  if (geminiProjectIdEdit->text().trimmed().isEmpty() && !result.clientConfig.projectId.isEmpty()) {
    geminiProjectIdEdit->setText(result.clientConfig.projectId);
  }

  SecretStorage::saveSecret(QString::fromLatin1(SETTING_GEMINI_OAUTH_TOKEN_JSON),
                            GoogleOAuthManager::serializeToken(result.token));
  setStatus(tr("Google login for Gemini is complete."));
  refreshContextState();
}

void AIPromptPanel::onGeminiSignOutClicked()
{
  const auto token = GoogleOAuthManager::loadToken(
    SecretStorage::loadSecret(QString::fromLatin1(SETTING_GEMINI_OAUTH_TOKEN_JSON)));
  const QString revokeError = GoogleOAuthManager::revokeToken(token);

  SecretStorage::clearSecret(QString::fromLatin1(SETTING_GEMINI_OAUTH_TOKEN_JSON));
  if (revokeError.isEmpty()) {
    setStatus(tr("Google login was removed for Gemini."));
  } else {
    setStatus(tr("Google login was removed locally, but token revocation failed: %1").arg(revokeError));
  }
  refreshContextState();
}

void AIPromptPanel::onShowKeysToggled(bool checked)
{
  const auto mode = checked ? QLineEdit::Normal : QLineEdit::PasswordEchoOnEdit;
  openAIKeyEdit->setEchoMode(mode);
  geminiKeyEdit->setEchoMode(mode);
}

void AIPromptPanel::saveCurrentProviderModel(const QString& model)
{
  QSettingsCached settings;
  settings.setValue(settingKeyForProviderModel(currentProvider()), model.trimmed());
}

void AIPromptPanel::saveCurrentProvider()
{
  QSettingsCached settings;
  settings.setValue(SETTING_PROVIDER, providerCombo->currentData().toString());
}

void AIPromptPanel::saveCurrentReasoning()
{
  QSettingsCached settings;
  settings.setValue(SETTING_REASONING, currentReasoningMode());
}

void AIPromptPanel::saveCurrentPermission()
{
  QSettingsCached settings;
  settings.setValue(SETTING_PERMISSION, currentPermissionMode());
}

void AIPromptPanel::saveContextOptions()
{
  QSettingsCached settings;
  settings.setValue(SETTING_INCLUDE_DOCUMENT, includeDocumentCheck->isChecked());
  settings.setValue(SETTING_INCLUDE_SELECTION, includeSelectionCheck->isChecked());
}

void AIPromptPanel::saveGeminiAuthMode()
{
  QSettingsCached settings;
  settings.setValue(SETTING_GEMINI_AUTH_MODE, geminiAuthModeCombo->currentData().toString());
}

void AIPromptPanel::saveGeminiOAuthSettings()
{
  QSettingsCached settings;
  settings.setValue(SETTING_GEMINI_OAUTH_CLIENT_FILE, geminiOAuthClientFileEdit->text().trimmed());
  settings.setValue(SETTING_GEMINI_PROJECT_ID, geminiProjectIdEdit->text().trimmed());
  refreshContextState();
}

void AIPromptPanel::updateProviderInstructions()
{
  QString instructions;
  if (currentProvider() == AIProvider::OpenAI) {
    instructions = tr("OpenAI uses an API key. Paste it below or set OPENAI_API_KEY, then click Generate.");
  } else if (usesGeminiOAuth()) {
    instructions = tr("Gemini's easy supported path is Google AI Studio plus an API key. This Advanced Google OAuth mode still requires a Desktop OAuth client JSON and a billed Google Cloud project.");
  } else {
    instructions = tr("Recommended Gemini setup: sign into Google AI Studio, create an API key, paste it below, or set GEMINI_API_KEY / GOOGLE_API_KEY. Advanced Google OAuth is optional.");
  }

  providerInstructionsLabel->setText(instructions);
}

void AIPromptPanel::updateReasoningHint()
{
  const auto provider = currentProvider();
  const QString model = currentModel();

  QString hint;
  if (provider == AIProvider::OpenAI) {
    hint = tr("Dynamic omits reasoning settings. Off maps to reasoning effort none; higher levels usually trade more latency for stronger code generation.");
  } else if (model.startsWith("gemini-3", Qt::CaseInsensitive)) {
    hint = tr("Gemini 3 uses thinkingLevel. Off maps to minimal on Flash models and is not allowed on Pro models.");
  } else {
    hint = tr("Gemini 2.5 uses thinking budgets. Off is not allowed on Pro models; Low, Medium, and High increase reasoning time and cost.");
  }
  reasoningHintLabel->setText(hint);
}
