#pragma once

#include <QString>

class SecretStorage
{
public:
  static QString loadSecret(const QString& key);
  static void saveSecret(const QString& key, const QString& secret);
  static void clearSecret(const QString& key);
};
