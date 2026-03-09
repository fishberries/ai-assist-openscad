#include "gui/SecretStorage.h"

#include <QSettings>
#include <cstring>

#include "gui/QSettingsCached.h"

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#endif

namespace {

constexpr auto SECRET_STORAGE_BACKEND_KEY = "ai/secretStorageBackend";
constexpr auto SECRET_STORAGE_LAST_ERROR_KEY = "ai/secretStorageLastError";

#ifdef _WIN32
QString credentialTarget(const QString& key)
{
  return QStringLiteral("OpenSCAD/%1").arg(key);
}

bool saveCredential(const QString& key, const QString& secret, DWORD *lastError = nullptr)
{
  const QByteArray secretBytes = secret.toUtf8();

  CREDENTIALW credential;
  memset(&credential, 0, sizeof(credential));
  credential.Type = CRED_TYPE_GENERIC;
  credential.Persist = CRED_PERSIST_ENTERPRISE;

  const std::wstring targetName = credentialTarget(key).toStdWString();
  credential.TargetName = const_cast<wchar_t *>(targetName.c_str());

  const std::wstring userName = L"OpenSCAD";
  credential.UserName = const_cast<wchar_t *>(userName.c_str());

  credential.CredentialBlobSize = static_cast<DWORD>(secretBytes.size());
  credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(secretBytes.constData()));

  const bool success = CredWriteW(&credential, 0) == TRUE;
  if (lastError != nullptr) {
    *lastError = success ? 0 : GetLastError();
  }
  return success;
}

QString loadCredential(const QString& key)
{
  PCREDENTIALW credential = nullptr;
  const std::wstring targetName = credentialTarget(key).toStdWString();
  if (!CredReadW(targetName.c_str(), CRED_TYPE_GENERIC, 0, &credential) || credential == nullptr) {
    return {};
  }

  const QString secret = QString::fromUtf8(
    reinterpret_cast<const char *>(credential->CredentialBlob),
    static_cast<int>(credential->CredentialBlobSize));
  CredFree(credential);
  return secret;
}

void clearCredential(const QString& key)
{
  const std::wstring targetName = credentialTarget(key).toStdWString();
  CredDeleteW(targetName.c_str(), CRED_TYPE_GENERIC, 0);
}
#endif

}  // namespace

QString SecretStorage::loadSecret(const QString& key)
{
  QSettings settings;
  QSettingsCached cachedSettings;

#ifdef _WIN32
  const QString credentialSecret = loadCredential(key);
  if (!credentialSecret.isEmpty()) return credentialSecret;
#endif

  const QString legacyValue = settings.value(key).toString();
  if (legacyValue.isEmpty()) return {};

#ifdef _WIN32
  DWORD lastError = 0;
  if (saveCredential(key, legacyValue, &lastError)) {
    settings.remove(key);
    cachedSettings.remove(key);
    settings.setValue(SECRET_STORAGE_BACKEND_KEY, QStringLiteral("windows-credential-manager"));
    settings.setValue(SECRET_STORAGE_LAST_ERROR_KEY, 0);
    settings.sync();
  } else {
    settings.setValue(SECRET_STORAGE_LAST_ERROR_KEY, static_cast<qulonglong>(lastError));
  }
#endif

  return legacyValue;
}

void SecretStorage::saveSecret(const QString& key, const QString& secret)
{
  QSettings settings;
  QSettingsCached cachedSettings;

  if (secret.isEmpty()) {
    clearSecret(key);
    return;
  }

#ifdef _WIN32
  DWORD lastError = 0;
  if (saveCredential(key, secret, &lastError)) {
    settings.remove(key);
    cachedSettings.remove(key);
    settings.setValue(SECRET_STORAGE_BACKEND_KEY, QStringLiteral("windows-credential-manager"));
    settings.setValue(SECRET_STORAGE_LAST_ERROR_KEY, 0);
    settings.sync();
    return;
  }
  settings.setValue(SECRET_STORAGE_LAST_ERROR_KEY, static_cast<qulonglong>(lastError));
#endif

  settings.setValue(SECRET_STORAGE_BACKEND_KEY, QStringLiteral("qsettings-fallback"));
  settings.setValue(key, secret);
}

void SecretStorage::clearSecret(const QString& key)
{
  QSettings settings;
  QSettingsCached cachedSettings;
#ifdef _WIN32
  clearCredential(key);
#endif
  settings.remove(key);
  cachedSettings.remove(key);
  settings.sync();
}
