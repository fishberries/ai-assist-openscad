#pragma once

#include <QDateTime>
#include <QString>

struct GoogleOAuthClientConfig
{
  QString clientId;
  QString clientSecret;
  QString projectId;
  QString authUri;
  QString tokenUri;
};

struct GoogleOAuthToken
{
  QString accessToken;
  QString refreshToken;
  QString tokenType;
  QString scope;
  QDateTime expiryUtc;

  bool hasUsableAccessToken() const;
};

struct GoogleOAuthResult
{
  bool success{false};
  QString error;
  GoogleOAuthClientConfig clientConfig;
  GoogleOAuthToken token;
};

class QWidget;

class GoogleOAuthManager
{
public:
  static GoogleOAuthResult loadClientConfig(const QString& clientSecretPath);
  static GoogleOAuthToken loadToken(const QString& tokenJson);
  static QString serializeToken(const GoogleOAuthToken& token);
  static GoogleOAuthResult authorizeForGemini(QWidget *parent, const QString& clientSecretPath);
  static GoogleOAuthResult refreshGeminiToken(const QString& clientSecretPath,
                                              const QString& storedTokenJson);
  static QString revokeToken(const GoogleOAuthToken& token);
};
