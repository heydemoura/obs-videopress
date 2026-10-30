// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "credential-store.hpp"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <QHash>
#endif

namespace credential_store {

#ifdef __APPLE__

namespace {

constexpr const char *kService = "com.videopress.obs-videopress";

struct CFReleaser {
	CFTypeRef ref;
	~CFReleaser()
	{
		if (ref)
			CFRelease(ref);
	}
};

CFStringRef cfString(const QString &value)
{
	const QByteArray utf8 = value.toUtf8();
	return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(utf8.constData()),
				       static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
}

// Base query: generic password for (service, account). Caller releases.
CFMutableDictionaryRef baseQuery(CFStringRef service, CFStringRef account)
{
	CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
								 &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(query, kSecAttrService, service);
	CFDictionarySetValue(query, kSecAttrAccount, account);
	return query;
}

QString describe(OSStatus status)
{
	CFStringRef message = SecCopyErrorMessageString(status, nullptr);
	CFReleaser releaser{message};
	if (!message)
		return QStringLiteral("Keychain error %1").arg(status);
	char buffer[512];
	if (!CFStringGetCString(message, buffer, sizeof(buffer), kCFStringEncodingUTF8))
		return QStringLiteral("Keychain error %1").arg(status);
	return QString::fromUtf8(buffer);
}

} // namespace

bool isPersistent()
{
	return true;
}

bool save(const QString &account, const QString &secret, QString *error)
{
	CFStringRef service = cfString(QString::fromLatin1(kService));
	CFStringRef accountRef = cfString(account);
	CFReleaser serviceReleaser{service}, accountReleaser{accountRef};

	const QByteArray secretUtf8 = secret.toUtf8();
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(secretUtf8.constData()),
				      static_cast<CFIndex>(secretUtf8.size()));
	CFReleaser dataReleaser{data};

	CFMutableDictionaryRef query = baseQuery(service, accountRef);
	CFReleaser queryReleaser{query};

	CFMutableDictionaryRef update = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFReleaser updateReleaser{update};
	CFDictionarySetValue(update, kSecValueData, data);

	OSStatus status = SecItemUpdate(query, update);
	if (status == errSecItemNotFound) {
		CFDictionarySetValue(query, kSecValueData, data);
		CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
		status = SecItemAdd(query, nullptr);
	}
	if (status != errSecSuccess) {
		if (error)
			*error = describe(status);
		return false;
	}
	return true;
}

QString load(const QString &account)
{
	CFStringRef service = cfString(QString::fromLatin1(kService));
	CFStringRef accountRef = cfString(account);
	CFReleaser serviceReleaser{service}, accountReleaser{accountRef};

	CFMutableDictionaryRef query = baseQuery(service, accountRef);
	CFReleaser queryReleaser{query};
	CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
	CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

	CFTypeRef result = nullptr;
	if (SecItemCopyMatching(query, &result) != errSecSuccess || !result)
		return QString();
	CFReleaser resultReleaser{result};
	const auto data = static_cast<CFDataRef>(result);
	return QString::fromUtf8(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
				 static_cast<qsizetype>(CFDataGetLength(data)));
}

void remove(const QString &account)
{
	CFStringRef service = cfString(QString::fromLatin1(kService));
	CFStringRef accountRef = cfString(account);
	CFReleaser serviceReleaser{service}, accountReleaser{accountRef};
	CFMutableDictionaryRef query = baseQuery(service, accountRef);
	CFReleaser queryReleaser{query};
	SecItemDelete(query);
}

#else

namespace {
QHash<QString, QString> &sessionStore()
{
	static QHash<QString, QString> store;
	return store;
}
} // namespace

bool isPersistent()
{
	return false;
}

bool save(const QString &account, const QString &secret, QString *)
{
	sessionStore().insert(account, secret);
	return true;
}

QString load(const QString &account)
{
	return sessionStore().value(account);
}

void remove(const QString &account)
{
	sessionStore().remove(account);
}

#endif

} // namespace credential_store
