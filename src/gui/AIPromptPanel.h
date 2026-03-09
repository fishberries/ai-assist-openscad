#pragma once

#include <QFutureWatcher>
#include <QWidget>

#include "gui/AICodeGenerator.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class MainWindow;

class AIPromptPanel : public QWidget
{
  Q_OBJECT

public:
  explicit AIPromptPanel(MainWindow *mainWindow, QWidget *parent = nullptr);
  ~AIPromptPanel() override = default;

public slots:
  void refreshContextState();

private slots:
  void onProviderChanged();
  void onGenerationFinished();
  void onGenerateClicked();
  void onApplyClicked();
  void onCopyClicked();
  void onGeminiAuthModeChanged();
  void onGeminiOAuthBrowseClicked();
  void onGeminiSignInClicked();
  void onGeminiSignOutClicked();
  void onShowKeysToggled(bool checked);
  void saveCurrentProviderModel(const QString& model);
  void saveCurrentProvider();
  void saveCurrentReasoning();
  void saveCurrentPermission();
  void saveContextOptions();
  void saveGeminiAuthMode();
  void saveGeminiOAuthSettings();
  void updateProviderInstructions();
  void updateReasoningHint();

private:
  void loadSettings();
  void populateModels();
  void setStatus(const QString& status);
  void setBusy(bool busy);
  void updateApplyButton();
  void updateCredentialVisibility();
  void updateGeminiLoginStatus();
  AIProvider currentProvider() const;
  QString currentApiKey() const;
  QString currentModel() const;
  QString currentReasoningMode() const;
  QString currentPermissionMode() const;
  bool usesGeminiOAuth() const;
  QString settingKeyForProviderModel(AIProvider provider) const;
  AIGenerationRequest buildRequest(QString *errorMessage = nullptr);

  MainWindow *mainWindow;
  QFutureWatcher<AIGenerationResult> generationWatcher;

  QComboBox *providerCombo{nullptr};
  QComboBox *geminiAuthModeCombo{nullptr};
  QLineEdit *openAIKeyEdit{nullptr};
  QLineEdit *geminiKeyEdit{nullptr};
  QLineEdit *geminiOAuthClientFileEdit{nullptr};
  QLineEdit *geminiProjectIdEdit{nullptr};
  QPushButton *geminiOAuthBrowseButton{nullptr};
  QPushButton *geminiSignInButton{nullptr};
  QPushButton *geminiSignOutButton{nullptr};
  QCheckBox *showKeysCheck{nullptr};
  QComboBox *modelCombo{nullptr};
  QComboBox *reasoningCombo{nullptr};
  QComboBox *permissionCombo{nullptr};
  QCheckBox *includeDocumentCheck{nullptr};
  QCheckBox *includeSelectionCheck{nullptr};
  QLabel *geminiLoginStatusLabel{nullptr};
  QLabel *providerInstructionsLabel{nullptr};
  QLabel *reasoningHintLabel{nullptr};
  QLabel *statusLabel{nullptr};
  QWidget *openAIKeyRowWidget{nullptr};
  QWidget *geminiQuickActionsRowWidget{nullptr};
  QWidget *geminiAuthModeRowWidget{nullptr};
  QWidget *geminiApiKeyRowWidget{nullptr};
  QWidget *geminiOAuthFileRowWidget{nullptr};
  QWidget *geminiProjectRowWidget{nullptr};
  QWidget *geminiLoginButtonsRowWidget{nullptr};
  QPlainTextEdit *promptEdit{nullptr};
  QPlainTextEdit *responseEdit{nullptr};
  QPushButton *generateButton{nullptr};
  QPushButton *applyButton{nullptr};
  QPushButton *copyButton{nullptr};

  QString lastGeneratedCode;
};
