#include "gui/GoogleOAuthManager.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include "gui/Network.h"

namespace {

constexpr auto AUTHORIZATION_ENDPOINT = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr auto TOKEN_ENDPOINT = "https://oauth2.googleapis.com/token";
constexpr auto REVOKE_ENDPOINT = "https://oauth2.googleapis.com/revoke";
constexpr int NETWORK_TIMEOUT_SECONDS = 60;
constexpr int LOGIN_TIMEOUT_MILLISECONDS = 300000;
constexpr auto GEMINI_SCOPE_RETRIEVER = "https://www.googleapis.com/auth/generative-language.retriever";
constexpr auto GEMINI_SCOPE_CLOUD = "https://www.googleapis.com/auth/cloud-platform";

QString errorMessage(const QByteArray& payload, const QString& fallback)
{
  const auto doc = QJsonDocument::fromJson(payload);
  const auto root = doc.object();
  const auto error = root.value("error");
  if (error.isObject()) {
    const auto object = error.toObject();
    const QString message = object.value("message").toString().trimmed();
    if (!message.isEmpty()) return message;
  }

  const QString directError = root.value("error_description").toString().trimmed();
  if (!directError.isEmpty()) return directError;

  return fallback.trimmed().isEmpty() ? QStringLiteral("Google OAuth request failed.")
                                      : fallback.trimmed();
}

QString base64Url(const QByteArray& data)
{
  return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString randomVerifier(int bytes)
{
  QByteArray buffer(bytes, Qt::Uninitialized);
  for (int i = 0; i < buffer.size(); ++i) {
    buffer[i] = static_cast<char>(QRandomGenerator::global()->generate() & 0xff);
  }
  return base64Url(buffer);
}

QString makeCodeChallenge(const QString& verifier)
{
  return base64Url(QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256));
}

GoogleOAuthResult failure(const QString& message)
{
  GoogleOAuthResult result;
  result.error = message;
  return result;
}

GoogleOAuthToken parseTokenObject(const QJsonObject& object, const GoogleOAuthToken& existing = {})
{
  GoogleOAuthToken token = existing;
  token.accessToken = object.value("access_token").toString(token.accessToken);
  token.refreshToken = object.value("refresh_token").toString(token.refreshToken);
  token.tokenType = object.value("token_type").toString(token.tokenType);
  token.scope = object.value("scope").toString(token.scope);

  const int expiresIn = object.value("expires_in").toInt(0);
  if (expiresIn > 0) {
    token.expiryUtc = QDateTime::currentDateTimeUtc().addSecs(expiresIn - 60);
  }

  return token;
}

GoogleOAuthResult exchangeForToken(const GoogleOAuthClientConfig& clientConfig, const QString& body,
                                   const GoogleOAuthToken& existing = {})
{
  auto networkRequest =
    NetworkRequest<QJsonObject>(QUrl(clientConfig.tokenUri), {200}, NETWORK_TIMEOUT_SECONDS);

  const auto reply = networkRequest.execute(
    [&](QNetworkRequest& network) {
      network.setHeader(QNetworkRequest::ContentTypeHeader,
                        "application/x-www-form-urlencoded");
    },
    [&](QNetworkAccessManager& manager, QNetworkRequest& network) {
      return manager.post(network, body.toUtf8());
    },
    [](QNetworkReply *reply) { return QJsonDocument::fromJson(reply->readAll()).object(); },
    [](QNetworkReply *reply) -> QJsonObject {
      throw NetworkException(reply->error(), errorMessage(reply->readAll(), reply->errorString()));
    });

  if (reply.contains("error")) {
    return failure(errorMessage(QJsonDocument(reply).toJson(QJsonDocument::Compact),
                                QStringLiteral("Google OAuth token exchange failed.")));
  }

  GoogleOAuthResult result;
  result.success = true;
  result.clientConfig = clientConfig;
  result.token = parseTokenObject(reply, existing);
  return result;
}

QUrl buildAuthorizationUrl(const GoogleOAuthClientConfig& clientConfig, const QString& redirectUri,
                           const QString& state, const QString& verifier)
{
  QUrl url(clientConfig.authUri);
  QUrlQuery query;
  query.addQueryItem("client_id", clientConfig.clientId);
  query.addQueryItem("redirect_uri", redirectUri);
  query.addQueryItem("response_type", "code");
  query.addQueryItem("scope",
                     QStringLiteral("%1 %2").arg(QString::fromLatin1(GEMINI_SCOPE_CLOUD),
                                                 QString::fromLatin1(GEMINI_SCOPE_RETRIEVER)));
  query.addQueryItem("state", state);
  query.addQueryItem("code_challenge", makeCodeChallenge(verifier));
  query.addQueryItem("code_challenge_method", "S256");
  query.addQueryItem("access_type", "offline");
  query.addQueryItem("prompt", "consent");
  url.setQuery(query);
  return url;
}

GoogleOAuthResult waitForAuthorizationCode(QWidget *, const GoogleOAuthClientConfig& clientConfig)
{
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost, 0)) {
    return failure(QStringLiteral("Could not start a local OAuth callback server on 127.0.0.1."));
  }

  const QString redirectUri = QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort());
  const QString state = randomVerifier(18);
  const QString verifier = randomVerifier(48);
  const QUrl authUrl = buildAuthorizationUrl(clientConfig, redirectUri, state, verifier);

  if (!QDesktopServices::openUrl(authUrl)) {
    return failure(QStringLiteral("OpenSCAD could not open the browser for Google login."));
  }

  QString receivedCode;
  QString receivedError;
  QString receivedState;

  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(&server, &QTcpServer::newConnection, &loop, [&]() {
    auto *socket = server.nextPendingConnection();
    if (socket == nullptr) return;
    if (!socket->waitForReadyRead(10000)) {
      socket->disconnectFromHost();
      socket->deleteLater();
      return;
    }

    const QByteArray request = socket->readAll();
    const QList<QByteArray> lines = request.split('\n');
    if (!lines.isEmpty()) {
      const QList<QByteArray> parts = lines.first().trimmed().split(' ');
      if (parts.size() >= 2) {
        const QUrl callbackUrl(QStringLiteral("http://127.0.0.1") + QString::fromUtf8(parts.at(1)));
        const QUrlQuery query(callbackUrl);
        receivedCode = query.queryItemValue("code");
        receivedError = query.queryItemValue("error");
        receivedState = query.queryItemValue("state");
      }
    }

    const QString body =
      receivedCode.isEmpty()
        ? QStringLiteral("<html><body><h3>Google login failed</h3><p>%1</p></body></html>")
            .arg(receivedError.isEmpty() ? QStringLiteral("No authorization code was returned.")
                                         : receivedError.toHtmlEscaped())
        : QStringLiteral("<html><body><h3>Google login complete</h3><p>You can return to OpenSCAD.</p></body></html>");
    const QByteArray response =
      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n" +
      body.toUtf8();
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
    socket->deleteLater();
    loop.quit();
  });

  timeout.start(LOGIN_TIMEOUT_MILLISECONDS);
  loop.exec();

  if (!timeout.isActive()) {
    return failure(QStringLiteral("Google login timed out before OpenSCAD received the callback."));
  }
  if (!receivedError.isEmpty()) {
    return failure(QStringLiteral("Google login failed: %1").arg(receivedError));
  }
  if (receivedCode.isEmpty()) {
    return failure(QStringLiteral("Google login did not return an authorization code."));
  }
  if (receivedState != state) {
    return failure(QStringLiteral("Google login state validation failed."));
  }

  QUrlQuery body;
  body.addQueryItem("client_id", clientConfig.clientId);
  if (!clientConfig.clientSecret.isEmpty()) {
    body.addQueryItem("client_secret", clientConfig.clientSecret);
  }
  body.addQueryItem("code", receivedCode);
  body.addQueryItem("code_verifier", verifier);
  body.addQueryItem("grant_type", "authorization_code");
  body.addQueryItem("redirect_uri", redirectUri);
  return exchangeForToken(clientConfig, body.toString(QUrl::FullyEncoded));
}

}  // namespace

bool GoogleOAuthToken::hasUsableAccessToken() const
{
  return !accessToken.isEmpty() && expiryUtc.isValid() &&
         expiryUtc > QDateTime::currentDateTimeUtc();
}

GoogleOAuthResult GoogleOAuthManager::loadClientConfig(const QString& clientSecretPath)
{
  QFile file(clientSecretPath);
  if (!file.exists()) {
    return failure(QStringLiteral("The Google OAuth client JSON file does not exist."));
  }
  if (!file.open(QIODevice::ReadOnly)) {
    return failure(QStringLiteral("The Google OAuth client JSON file could not be opened."));
  }

  const auto document = QJsonDocument::fromJson(file.readAll());
  if (!document.isObject()) {
    return failure(QStringLiteral("The Google OAuth client JSON file is not valid JSON."));
  }

  const auto root = document.object();
  QJsonObject credentials = root.value("installed").toObject();
  if (credentials.isEmpty()) {
    credentials = root.value("web").toObject();
  }
  if (credentials.isEmpty()) {
    credentials = root;
  }

  GoogleOAuthClientConfig config;
  config.clientId = credentials.value("client_id").toString();
  config.clientSecret = credentials.value("client_secret").toString();
  config.projectId = credentials.value("project_id").toString(root.value("project_id").toString());
  config.authUri = credentials.value("auth_uri").toString(QString::fromLatin1(AUTHORIZATION_ENDPOINT));
  config.tokenUri = credentials.value("token_uri").toString(QString::fromLatin1(TOKEN_ENDPOINT));

  if (config.clientId.isEmpty()) {
    return failure(QStringLiteral("The Google OAuth client JSON file does not contain a client_id."));
  }

  GoogleOAuthResult result;
  result.success = true;
  result.clientConfig = config;
  return result;
}

GoogleOAuthToken GoogleOAuthManager::loadToken(const QString& tokenJson)
{
  if (tokenJson.trimmed().isEmpty()) return {};
  const auto document = QJsonDocument::fromJson(tokenJson.toUtf8());
  if (!document.isObject()) return {};
  auto token = parseTokenObject(document.object());
  const QString expiry = document.object().value("expiry_utc").toString();
  if (!expiry.isEmpty()) {
    token.expiryUtc = QDateTime::fromString(expiry, Qt::ISODateWithMs);
    if (!token.expiryUtc.isValid()) {
      token.expiryUtc = QDateTime::fromString(expiry, Qt::ISODate);
    }
  }
  return token;
}

QString GoogleOAuthManager::serializeToken(const GoogleOAuthToken& token)
{
  QJsonObject object;
  object.insert("access_token", token.accessToken);
  object.insert("refresh_token", token.refreshToken);
  object.insert("token_type", token.tokenType);
  object.insert("scope", token.scope);
  if (token.expiryUtc.isValid()) {
    object.insert("expiry_utc", token.expiryUtc.toString(Qt::ISODateWithMs));
  }
  return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

GoogleOAuthResult GoogleOAuthManager::authorizeForGemini(QWidget *parent,
                                                         const QString& clientSecretPath)
{
  auto configResult = loadClientConfig(clientSecretPath);
  if (!configResult.success) return configResult;
  return waitForAuthorizationCode(parent, configResult.clientConfig);
}

GoogleOAuthResult GoogleOAuthManager::refreshGeminiToken(const QString& clientSecretPath,
                                                         const QString& storedTokenJson)
{
  auto configResult = loadClientConfig(clientSecretPath);
  if (!configResult.success) return configResult;

  GoogleOAuthToken existing = loadToken(storedTokenJson);
  if (existing.hasUsableAccessToken()) {
    GoogleOAuthResult result;
    result.success = true;
    result.clientConfig = configResult.clientConfig;
    result.token = existing;
    return result;
  }

  if (existing.refreshToken.isEmpty()) {
    return failure(QStringLiteral("No Google refresh token is stored. Sign in again."));
  }

  QUrlQuery body;
  body.addQueryItem("client_id", configResult.clientConfig.clientId);
  if (!configResult.clientConfig.clientSecret.isEmpty()) {
    body.addQueryItem("client_secret", configResult.clientConfig.clientSecret);
  }
  body.addQueryItem("refresh_token", existing.refreshToken);
  body.addQueryItem("grant_type", "refresh_token");
  return exchangeForToken(configResult.clientConfig, body.toString(QUrl::FullyEncoded), existing);
}

QString GoogleOAuthManager::revokeToken(const GoogleOAuthToken& token)
{
  const QString tokenToRevoke =
    !token.refreshToken.isEmpty() ? token.refreshToken : token.accessToken;
  if (tokenToRevoke.isEmpty()) return {};

  QUrl revokeUrl(QString::fromLatin1(REVOKE_ENDPOINT));
  QUrlQuery revokeQuery;
  revokeQuery.addQueryItem("token", tokenToRevoke);
  revokeUrl.setQuery(revokeQuery);

  auto networkRequest =
    NetworkRequest<QString>(revokeUrl, {200}, NETWORK_TIMEOUT_SECONDS);
  try {
    return networkRequest.execute(
      [](QNetworkRequest&) {},
      [](QNetworkAccessManager& manager, QNetworkRequest& network) { return manager.post(network, QByteArray()); },
      [](QNetworkReply *) { return QString(); },
      [](QNetworkReply *reply) -> QString {
        throw NetworkException(reply->error(), errorMessage(reply->readAll(), reply->errorString()));
      });
  } catch (const NetworkException& error) {
    return QString::fromStdString(error.getErrorMessage());
  } catch (const std::exception& error) {
    return QString::fromStdString(error.what());
  }
}
