/*
 * Copyright (C) by Klaas Freitag <freitag@kde.org>
 * Copyright (C) by Krzesimir Nowak <krzesimir@endocode.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QLoggingCategory>
#include <QMutex>
#include <QNetworkReply>
#include <QSettings>
#include <QSslKey>

#include <keychain.h>

#include "account.h"
#include "accessmanager.h"
#include "utility.h"
#include "theme.h"
#include "syncengine.h"
#include "creds/credentialscommon.h"
#include "creds/httpcredentials.h"

using namespace QKeychain;

namespace OCC {

Q_LOGGING_CATEGORY(lcHttpCredentials, "sync.credentials.http", QtInfoMsg)

namespace {
    const char userC[] = "user";
    const char clientCertificatePEMC[] = "_clientCertificatePEM";
    const char clientKeyPEMC[] = "_clientKeyPEM";
    const char authenticationFailedC[] = "owncloud-authentication-failed";
} // ns

class HttpCredentialsAccessManager : public AccessManager
{
public:
    HttpCredentialsAccessManager(const HttpCredentials *cred, QObject *parent = 0)
        : AccessManager(parent)
        , _cred(cred)
    {
    }

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) Q_DECL_OVERRIDE
    {
        QByteArray credHash = QByteArray(_cred->user().toUtf8() + ":" + _cred->password().toUtf8()).toBase64();
        QNetworkRequest req(request);
        req.setRawHeader(QByteArray("Authorization"), QByteArray("Basic ") + credHash);

        if (!_cred->_clientSslKey.isNull() && !_cred->_clientSslCertificate.isNull()) {
            // SSL configuration
            QSslConfiguration sslConfiguration = req.sslConfiguration();
            sslConfiguration.setLocalCertificate(_cred->_clientSslCertificate);
            sslConfiguration.setPrivateKey(_cred->_clientSslKey);
            req.setSslConfiguration(sslConfiguration);
        }


        return AccessManager::createRequest(op, req, outgoingData);
    }

private:
    const HttpCredentials *_cred;
};


static void addSettingsToJob(Account *account, QKeychain::Job *job)
{
    Q_UNUSED(account);
    auto settings = Utility::settingsWithGroup(Theme::instance()->appName());
    settings->setParent(job); // make the job parent to make setting deleted properly
    job->setSettings(settings.release());
}

HttpCredentials::HttpCredentials()
    : _ready(false)
{
}

// From wizard
HttpCredentials::HttpCredentials(const QString &user, const QString &password, const QSslCertificate &certificate, const QSslKey &key)
    : _user(user)
    , _password(password)
    , _ready(true)
    , _clientSslKey(key)
    , _clientSslCertificate(certificate)
{
}

QString HttpCredentials::authType() const
{
    return QString::fromLatin1("http");
}

QString HttpCredentials::user() const
{
    return _user;
}

QString HttpCredentials::password() const
{
    return _password;
}

void HttpCredentials::setAccount(Account *account)
{
    AbstractCredentials::setAccount(account);
    if (_user.isEmpty()) {
        fetchUser();
    }
}

QNetworkAccessManager *HttpCredentials::getQNAM() const
{
    AccessManager *qnam = new HttpCredentialsAccessManager(this);

    connect(qnam, SIGNAL(authenticationRequired(QNetworkReply *, QAuthenticator *)),
        this, SLOT(slotAuthentication(QNetworkReply *, QAuthenticator *)));

    return qnam;
}

bool HttpCredentials::ready() const
{
    return _ready;
}

QString HttpCredentials::fetchUser()
{
    _user = _account->credentialSetting(QLatin1String(userC)).toString();
    return _user;
}

void HttpCredentials::fetchFromKeychain()
{
    _wasFetched = true;

    // User must be fetched from config file
    fetchUser();

    const QString kck = keychainKey(_account->url().toString(), _user);

    if (_ready) {
        Q_EMIT fetched();
    } else {
        // Read client cert from keychain
        const QString kck = keychainKey(_account->url().toString(), _user + clientCertificatePEMC);
        ReadPasswordJob *job = new ReadPasswordJob(Theme::instance()->appName());
        addSettingsToJob(_account, job);
        job->setInsecureFallback(false);
        job->setKey(kck);
        qCDebug(lcHttpCredentials) << "-------- ----->" << _clientSslCertificate << _clientSslKey;

        connect(job, SIGNAL(finished(QKeychain::Job *)), SLOT(slotReadClientCertPEMJobDone(QKeychain::Job *)));
        job->start();
    }
}

void HttpCredentials::slotReadClientCertPEMJobDone(QKeychain::Job *incoming)
{
    // Store PEM in memory
    ReadPasswordJob *readJob = static_cast<ReadPasswordJob *>(incoming);
    if (readJob->error() == NoError && readJob->binaryData().length() > 0) {
        QList<QSslCertificate> sslCertificateList = QSslCertificate::fromData(readJob->binaryData(), QSsl::Pem);
        if (sslCertificateList.length() >= 1) {
            _clientSslCertificate = sslCertificateList.at(0);
        }
    }

    // Load key too
    const QString kck = keychainKey(_account->url().toString(), _user + clientKeyPEMC);
    ReadPasswordJob *job = new ReadPasswordJob(Theme::instance()->appName());
    addSettingsToJob(_account, job);
    job->setInsecureFallback(false);
    job->setKey(kck);

    connect(job, SIGNAL(finished(QKeychain::Job *)), SLOT(slotReadClientKeyPEMJobDone(QKeychain::Job *)));
    job->start();
}

void HttpCredentials::slotReadClientKeyPEMJobDone(QKeychain::Job *incoming)
{
    // Store key in memory
    ReadPasswordJob *readJob = static_cast<ReadPasswordJob *>(incoming);

    if (readJob->error() == NoError && readJob->binaryData().length() > 0) {
        QByteArray clientKeyPEM = readJob->binaryData();
        // FIXME Unfortunately Qt has a bug and we can't just use QSsl::Opaque to let it
        // load whatever we have. So we try until it works.
        _clientSslKey = QSslKey(clientKeyPEM, QSsl::Rsa);
        if (_clientSslKey.isNull()) {
            _clientSslKey = QSslKey(clientKeyPEM, QSsl::Dsa);
        }
#if QT_VERSION >= QT_VERSION_CHECK(5, 5, 0)
        // ec keys are Qt 5.5
        if (_clientSslKey.isNull()) {
            _clientSslKey = QSslKey(clientKeyPEM, QSsl::Ec);
        }
#endif
        if (_clientSslKey.isNull()) {
            qCWarning(lcHttpCredentials) << "Could not load SSL key into Qt!";
        }
    }

    // Now fetch the actual server password
    const QString kck = keychainKey(_account->url().toString(), _user);
    ReadPasswordJob *job = new ReadPasswordJob(Theme::instance()->appName());
    addSettingsToJob(_account, job);
    job->setInsecureFallback(false);
    job->setKey(kck);

    connect(job, SIGNAL(finished(QKeychain::Job *)), SLOT(slotReadJobDone(QKeychain::Job *)));
    job->start();
}


bool HttpCredentials::stillValid(QNetworkReply *reply)
{
    return ((reply->error() != QNetworkReply::AuthenticationRequiredError)
        // returned if user or password is incorrect
        && (reply->error() != QNetworkReply::OperationCanceledError
               || !reply->property(authenticationFailedC).toBool()));
}

void HttpCredentials::slotReadJobDone(QKeychain::Job *incomingJob)
{
    QKeychain::ReadPasswordJob *job = static_cast<ReadPasswordJob *>(incomingJob);
    _password = job->textData();

    if (_user.isEmpty()) {
        qCWarning(lcHttpCredentials) << "Strange: User is empty!";
    }

    QKeychain::Error error = job->error();

    if (!_password.isEmpty() && error == NoError) {
        // All cool, the keychain did not come back with error.
        // Still, the password can be empty which indicates a problem and
        // the password dialog has to be opened.
        _ready = true;
        emit fetched();
    } else {
        // we come here if the password is empty or any other keychain
        // error happend.

        _fetchErrorString = job->error() != EntryNotFound ? job->errorString() : QString();

        _password = QString();
        _ready = false;
        emit fetched();
    }
}

void HttpCredentials::invalidateToken()
{
    if (!_password.isEmpty()) {
        _previousPassword = _password;
    }
    _password = QString();
    _ready = false;

    // User must be fetched from config file to generate a valid key
    fetchUser();

    const QString kck = keychainKey(_account->url().toString(), _user);
    if (kck.isEmpty()) {
        qCWarning(lcHttpCredentials) << "InvalidateToken: User is empty, bailing out!";
        return;
    }

    DeletePasswordJob *job = new DeletePasswordJob(Theme::instance()->appName());
    addSettingsToJob(_account, job);
    job->setInsecureFallback(true);
    job->setKey(kck);
    job->start();

    // Also ensure the password is deleted from the deprecated place
    // otherwise we'd possibly read and use it again and again.
    DeletePasswordJob *job2 = new DeletePasswordJob(Theme::instance()->appName());
    // no job2->setSettings() call here, to make it use the deprecated location.
    job2->setInsecureFallback(true);
    job2->setKey(kck);
    job2->start();

    // clear the session cookie.
    _account->clearCookieJar();

    // let QNAM forget about the password
    // This needs to be done later in the event loop because we might be called (directly or
    // indirectly) from QNetworkAccessManagerPrivate::authenticationRequired, which itself
    // is a called from a BlockingQueuedConnection from the Qt HTTP thread. And clearing the
    // cache needs to synchronize again with the HTTP thread.
    QTimer::singleShot(0, this, SLOT(clearQNAMCache()));
}

void HttpCredentials::clearQNAMCache()
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    _account->networkAccessManager()->clearAccessCache();
#else
    _account->resetNetworkAccessManager();
#endif
}

void HttpCredentials::forgetSensitiveData()
{
    invalidateToken();
    _previousPassword.clear();
}

void HttpCredentials::persist()
{
    if (_user.isEmpty()) {
        // We never connected or fetched the user, there is nothing to save.
        return;
    }

    _account->setCredentialSetting(QLatin1String(userC), _user);

    // write cert
    WritePasswordJob *job = new WritePasswordJob(Theme::instance()->appName());
    addSettingsToJob(_account, job);
    job->setInsecureFallback(false);
    connect(job, SIGNAL(finished(QKeychain::Job *)), SLOT(slotWriteClientCertPEMJobDone(QKeychain::Job *)));
    job->setKey(keychainKey(_account->url().toString(), _user + clientCertificatePEMC));
    job->setBinaryData(_clientSslCertificate.toPem());
    job->start();
}

void HttpCredentials::slotWriteClientCertPEMJobDone(Job *incomingJob)
{
    Q_UNUSED(incomingJob);
    // write ssl key
    WritePasswordJob *job = new WritePasswordJob(Theme::instance()->appName());
    addSettingsToJob(_account, job);
    job->setInsecureFallback(false);
    connect(job, SIGNAL(finished(QKeychain::Job *)), SLOT(slotWriteClientKeyPEMJobDone(QKeychain::Job *)));
    job->setKey(keychainKey(_account->url().toString(), _user + clientKeyPEMC));
    job->setBinaryData(_clientSslKey.toPem());
    job->start();
}

void HttpCredentials::slotWriteClientKeyPEMJobDone(Job *incomingJob)
{
    Q_UNUSED(incomingJob);
    WritePasswordJob *job = new WritePasswordJob(Theme::instance()->appName());
    addSettingsToJob(_account, job);
    job->setInsecureFallback(false);
    connect(job, SIGNAL(finished(QKeychain::Job *)), SLOT(slotWriteJobDone(QKeychain::Job *)));
    job->setKey(keychainKey(_account->url().toString(), _user));
    job->setTextData(_password);
    job->start();
}

void HttpCredentials::slotWriteJobDone(QKeychain::Job *job)
{
    delete job->settings();
    switch (job->error()) {
    case NoError:
        break;
    default:
        qCWarning(lcHttpCredentials) << "Error while writing password" << job->errorString();
    }
    WritePasswordJob *wjob = qobject_cast<WritePasswordJob *>(job);
    wjob->deleteLater();
}

void HttpCredentials::slotAuthentication(QNetworkReply *reply, QAuthenticator *authenticator)
{
    Q_UNUSED(authenticator)
    // Because of issue #4326, we need to set the login and password manually at every requests
    // Thus, if we reach this signal, those credentials were invalid and we terminate.
    qCWarning(lcHttpCredentials) << "Stop request: Authentication failed for " << reply->url().toString();
    reply->setProperty(authenticationFailedC, true);
    reply->close();
}

} // namespace OCC
