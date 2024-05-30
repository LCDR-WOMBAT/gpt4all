#include "download.h"
#include "network.h"
#include "modellist.h"
#include "mysettings.h"

#include <QCoreApplication>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>

class MyDownload: public Download { };
Q_GLOBAL_STATIC(MyDownload, downloadInstance)
Download *Download::globalInstance()
{
    return downloadInstance();
}

Download::Download()
    : QObject(nullptr)
    , m_hashAndSave(new HashAndSaveFile)
{
    connect(this, &Download::requestHashAndSave, m_hashAndSave,
        &HashAndSaveFile::hashAndSave, Qt::QueuedConnection);
    connect(m_hashAndSave, &HashAndSaveFile::hashAndSaveFinished, this,
        &Download::handleHashAndSaveFinished, Qt::QueuedConnection);
    connect(&m_networkManager, &QNetworkAccessManager::sslErrors, this,
        &Download::handleSslErrors);
    updateReleaseNotes();
    m_startTime = QDateTime::currentDateTime();
}

static bool operator==(const ReleaseInfo& lhs, const ReleaseInfo& rhs) {
    return lhs.version == rhs.version;
}

static bool compareVersions(const QString &a, const QString &b) {
    QStringList aParts = a.split('.');
    QStringList bParts = b.split('.');

    for (int i = 0; i < std::min(aParts.size(), bParts.size()); ++i) {
        int aInt = aParts[i].toInt();
        int bInt = bParts[i].toInt();

        if (aInt > bInt) {
            return true;
        } else if (aInt < bInt) {
            return false;
        }
    }

    return aParts.size() > bParts.size();
}

ReleaseInfo Download::releaseInfo() const
{
    const QString currentVersion = QCoreApplication::applicationVersion();
    if (m_releaseMap.contains(currentVersion))
        return m_releaseMap.value(currentVersion);
    if (!m_releaseMap.empty())
        return m_releaseMap.last();
    return ReleaseInfo();
}

bool Download::hasNewerRelease() const
{
    const QString currentVersion = QCoreApplication::applicationVersion();
    QList<QString> versions = m_releaseMap.keys();
    std::sort(versions.begin(), versions.end(), compareVersions);
    if (versions.isEmpty())
        return false;
    return compareVersions(versions.first(), currentVersion);
}

bool Download::isFirstStart(bool writeVersion) const
{
    auto *mySettings = MySettings::globalInstance();

    QSettings settings;
    settings.sync();
    QString lastVersionStarted = settings.value("download/lastVersionStarted").toString();
    bool first = lastVersionStarted != QCoreApplication::applicationVersion();
    if (first && writeVersion) {
        settings.setValue("download/lastVersionStarted", QCoreApplication::applicationVersion());
        // let the user select these again
        settings.remove("network/usageStatsActive");
        settings.remove("network/isActive");
        settings.sync();
        emit mySettings->networkUsageStatsActiveChanged();
        emit mySettings->networkIsActiveChanged();
    }

    return first || !mySettings->isNetworkUsageStatsActiveSet() || !mySettings->isNetworkIsActiveSet();
}

void Download::updateReleaseNotes()
{
    QUrl jsonUrl("http://gpt4all.io/meta/release.json");
    QNetworkRequest request(jsonUrl);
    QSslConfiguration conf = request.sslConfiguration();
    conf.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(conf);
    QNetworkReply *jsonReply = m_networkManager.get(request);
    connect(qApp, &QCoreApplication::aboutToQuit, jsonReply, &QNetworkReply::abort);
    connect(jsonReply, &QNetworkReply::finished, this, &Download::handleReleaseJsonDownloadFinished);
}

void Download::downloadModel(const QString &modelFile)
{
    QFile *tempFile = new QFile(ModelList::globalInstance()->incompleteDownloadPath(modelFile));
    QDateTime modTime = tempFile->fileTime(QFile::FileModificationTime);
    bool success = tempFile->open(QIODevice::WriteOnly | QIODevice::Append);
    qWarning() << "Opening temp file for writing:" << tempFile->fileName();
    if (!success) {
        const QString error
            = QString("ERROR: Could not open temp file: %1 %2").arg(tempFile->fileName()).arg(modelFile);
        qWarning() << error;
        clearRetry(modelFile);
        ModelList::globalInstance()->updateDataByFilename(modelFile, {{ ModelList::DownloadErrorRole, error }});
        return;
    }
    tempFile->flush();
    size_t incomplete_size = tempFile->size();
    if (incomplete_size > 0) {
        bool success = tempFile->seek(incomplete_size);
        if (!success) {
            incomplete_size = 0;
            success = tempFile->seek(incomplete_size);
            Q_ASSERT(success);
        }
    }

    if (!ModelList::globalInstance()->containsByFilename(modelFile)) {
        qWarning() << "ERROR: Could not find file:" << modelFile;
        return;
    }

    ModelList::globalInstance()->updateDataByFilename(modelFile, {{ ModelList::DownloadingRole, true }});
    ModelInfo info = ModelList::globalInstance()->modelInfoByFilename(modelFile);
    QString url = !info.url().isEmpty() ? info.url() : "http://gpt4all.io/models/gguf/" + modelFile;
    Network::globalInstance()->trackEvent("download_started", { {"model", modelFile} });
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::User, modelFile);
    request.setRawHeader("range", QString("bytes=%1-").arg(tempFile->pos()).toUtf8());
    QSslConfiguration conf = request.sslConfiguration();
    conf.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(conf);
    QNetworkReply *modelReply = m_networkManager.get(request);
    connect(qApp, &QCoreApplication::aboutToQuit, modelReply, &QNetworkReply::abort);
    connect(modelReply, &QNetworkReply::downloadProgress, this, &Download::handleDownloadProgress);
    connect(modelReply, &QNetworkReply::errorOccurred, this, &Download::handleErrorOccurred);
    connect(modelReply, &QNetworkReply::finished, this, &Download::handleModelDownloadFinished);
    connect(modelReply, &QNetworkReply::readyRead, this, &Download::handleReadyRead);
    m_activeDownloads.insert(modelReply, tempFile);
}

void Download::cancelDownload(const QString &modelFile)
{
    for (int i = 0; i < m_activeDownloads.size(); ++i) {
        QNetworkReply *modelReply = m_activeDownloads.keys().at(i);
        QUrl url = modelReply->request().url();
        if (url.toString().endsWith(modelFile)) {
            Network::globalInstance()->trackEvent("download_canceled", { {"model", modelFile} });

            // Disconnect the signals
            disconnect(modelReply, &QNetworkReply::downloadProgress, this, &Download::handleDownloadProgress);
            disconnect(modelReply, &QNetworkReply::finished, this, &Download::handleModelDownloadFinished);

            modelReply->abort(); // Abort the download
            modelReply->deleteLater(); // Schedule the reply for deletion

            QFile *tempFile = m_activeDownloads.value(modelReply);
            tempFile->deleteLater();
            m_activeDownloads.remove(modelReply);

            ModelList::globalInstance()->updateDataByFilename(modelFile, {{ ModelList::DownloadingRole, false }});
            break;
        }
    }
}

void Download::installModel(const QString &modelFile, const QString &apiKey)
{
    Q_ASSERT(!apiKey.isEmpty());
    if (apiKey.isEmpty())
        return;

    Network::globalInstance()->trackEvent("install_model", { {"model", modelFile} });

    QString filePath = MySettings::globalInstance()->modelPath() + modelFile;
    QFile file(filePath);
    if (file.open(QIODeviceBase::WriteOnly | QIODeviceBase::Text)) {

        QJsonObject obj;
        QString modelName(modelFile);
        modelName.remove(0, 8); // strip "gpt4all-" prefix
        modelName.chop(7); // strip ".rmodel" extension
        obj.insert("apiKey", apiKey);
        obj.insert("modelName", modelName);
        QJsonDocument doc(obj);

        QTextStream stream(&file);
        stream << doc.toJson();
        file.close();
        ModelList::globalInstance()->updateModelsFromDirectory();
    }

    ModelList::globalInstance()->updateDataByFilename(modelFile, {{ ModelList::InstalledRole, true }});
}

void Download::removeModel(const QString &modelFile)
{
    const QString filePath = MySettings::globalInstance()->modelPath() + modelFile;
    QFile incompleteFile(ModelList::globalInstance()->incompleteDownloadPath(modelFile));
    if (incompleteFile.exists()) {
        incompleteFile.remove();
    }

    bool shouldRemoveInstalled = false;
    QFile file(filePath);
    if (file.exists()) {
        const ModelInfo info = ModelList::globalInstance()->modelInfoByFilename(modelFile);
        MySettings::globalInstance()->eraseModel(info);
        shouldRemoveInstalled = info.installed && !info.isClone() && (info.isDiscovered() || info.description() == "" /*indicates sideloaded*/);
        if (shouldRemoveInstalled)
            ModelList::globalInstance()->removeInstalled(info);
        Network::globalInstance()->trackEvent("remove_model", { {"model", modelFile} });
        file.remove();
    }

    if (!shouldRemoveInstalled) {
        QVector<QPair<int, QVariant>> data {
            { ModelList::InstalledRole, false },
            { ModelList::BytesReceivedRole, 0 },
            { ModelList::BytesTotalRole, 0 },
            { ModelList::TimestampRole, 0 },
            { ModelList::SpeedRole, QString() },
            { ModelList::DownloadErrorRole, QString() },
        };
        ModelList::globalInstance()->updateDataByFilename(modelFile, data);
    }
}

void Download::handleSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
    QUrl url = reply->request().url();
    for (const auto &e : errors)
        qWarning() << "ERROR: Received ssl error:" << e.errorString() << "for" << url;
}

void Download::handleReleaseJsonDownloadFinished()
{
    QNetworkReply *jsonReply = qobject_cast<QNetworkReply *>(sender());
    if (!jsonReply)
        return;

    QByteArray jsonData = jsonReply->readAll();
    jsonReply->deleteLater();
    parseReleaseJsonFile(jsonData);
}

void Download::parseReleaseJsonFile(const QByteArray &jsonData)
{
    QJsonParseError err;
    QJsonDocument document = QJsonDocument::fromJson(jsonData, &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "ERROR: Couldn't parse: " << jsonData << err.errorString();
        return;
    }

    QJsonArray jsonArray = document.array();

    m_releaseMap.clear();
    for (const QJsonValue &value : jsonArray) {
        QJsonObject obj = value.toObject();

        QString version = obj["version"].toString();
        QString notes = obj["notes"].toString();
        QString contributors = obj["contributors"].toString();
        ReleaseInfo releaseInfo;
        releaseInfo.version = version;
        releaseInfo.notes = notes;
        releaseInfo.contributors = contributors;
        m_releaseMap.insert(version, releaseInfo);
    }

    emit hasNewerReleaseChanged();
    emit releaseInfoChanged();
}

bool Download::hasRetry(const QString &filename) const
{
    return m_activeRetries.contains(filename);
}

bool Download::shouldRetry(const QString &filename)
{
    int retries = 0;
    if (m_activeRetries.contains(filename))
        retries = m_activeRetries.value(filename);

    ++retries;

    // Allow up to ten retries for now
    if (retries < 10) {
        m_activeRetries.insert(filename, retries);
        return true;
    }

    return false;
}

void Download::clearRetry(const QString &filename)
{
    m_activeRetries.remove(filename);
}

void Download::handleErrorOccurred(QNetworkReply::NetworkError code)
{
    QNetworkReply *modelReply = qobject_cast<QNetworkReply *>(sender());
    if (!modelReply)
        return;

    // This occurs when the user explicitly cancels the download
    if (code == QNetworkReply::OperationCanceledError)
        return;

    QString modelFilename = modelReply->request().attribute(QNetworkRequest::User).toString();
    if (shouldRetry(modelFilename)) {
        downloadModel(modelFilename);
        return;
    }

    clearRetry(modelFilename);

    const QString error
        = QString("ERROR: Network error occurred attempting to download %1 code: %2 errorString %3")
            .arg(modelFilename)
            .arg(code)
            .arg(modelReply->errorString());
    qWarning() << error;
    ModelList::globalInstance()->updateDataByFilename(modelFilename, {{ ModelList::DownloadErrorRole, error }});
    Network::globalInstance()->trackEvent("download_error", {
        {"model", modelFilename},
        {"code", (int)code},
        {"error", modelReply->errorString()},
    });
    cancelDownload(modelFilename);
}

void Download::handleDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    QNetworkReply *modelReply = qobject_cast<QNetworkReply *>(sender());
    if (!modelReply)
        return;
    QFile *tempFile = m_activeDownloads.value(modelReply);
    if (!tempFile)
        return;
    QString contentRange = modelReply->rawHeader("content-range");
    if (contentRange.contains("/")) {
        QString contentTotalSize = contentRange.split("/").last();
        bytesTotal = contentTotalSize.toLongLong();
    }

    const QString modelFilename = modelReply->request().attribute(QNetworkRequest::User).toString();
    const qint64 lastUpdate = ModelList::globalInstance()->dataByFilename(modelFilename, ModelList::TimestampRole).toLongLong();
    const qint64 currentUpdate = QDateTime::currentMSecsSinceEpoch();
    if (currentUpdate - lastUpdate < 1000)
        return;

    const qint64 lastBytesReceived = ModelList::globalInstance()->dataByFilename(modelFilename, ModelList::BytesReceivedRole).toLongLong();
    const qint64 currentBytesReceived = tempFile->pos();

    qint64 timeDifference = currentUpdate - lastUpdate;
    qint64 bytesDifference = currentBytesReceived - lastBytesReceived;
    qint64 speed = (bytesDifference / timeDifference) * 1000; // bytes per second
    QString speedText;
    if (speed < 1024)
        speedText = QString::number(static_cast<double>(speed), 'f', 2) + " B/s";
    else if (speed < 1024 * 1024)
        speedText = QString::number(static_cast<double>(speed / 1024.0), 'f', 2) + " KB/s";
    else
        speedText = QString::number(static_cast<double>(speed / (1024.0 * 1024.0)), 'f', 2) + " MB/s";

    QVector<QPair<int, QVariant>> data {
        { ModelList::BytesReceivedRole, currentBytesReceived },
        { ModelList::BytesTotalRole, bytesTotal },
        { ModelList::SpeedRole, speedText },
        { ModelList::TimestampRole, currentUpdate },
    };
    ModelList::globalInstance()->updateDataByFilename(modelFilename, data);
}

HashAndSaveFile::HashAndSaveFile()
    : QObject(nullptr)
{
    moveToThread(&m_hashAndSaveThread);
    m_hashAndSaveThread.setObjectName("hashandsave thread");
    m_hashAndSaveThread.start();
}

void HashAndSaveFile::hashAndSave(const QString &expectedHash, QCryptographicHash::Algorithm a,
    const QString &saveFilePath, QFile *tempFile, QNetworkReply *modelReply)
{
    Q_ASSERT(!tempFile->isOpen());
    QString modelFilename = modelReply->request().attribute(QNetworkRequest::User).toString();

    // Reopen the tempFile for hashing
    if (!tempFile->open(QIODevice::ReadOnly)) {
        const QString error
            = QString("ERROR: Could not open temp file for hashing: %1 %2").arg(tempFile->fileName()).arg(modelFilename);
        qWarning() << error;
        emit hashAndSaveFinished(false, error, tempFile, modelReply);
        return;
    }

    QCryptographicHash hash(a);
    while(!tempFile->atEnd())
        hash.addData(tempFile->read(16384));
    if (hash.result().toHex() != expectedHash.toLatin1()) {
        tempFile->close();
        const QString error
            = QString("ERROR: Download error hash did not match: %1 != %2 for %3")
                .arg(hash.result().toHex())
                .arg(expectedHash.toLatin1())
                .arg(modelFilename);
        qWarning() << error;
        tempFile->remove();
        emit hashAndSaveFinished(false, error, tempFile, modelReply);
        return;
    }

    // The file save needs the tempFile closed
    tempFile->close();

    // Attempt to *move* the verified tempfile into place - this should be atomic
    // but will only work if the destination is on the same filesystem
    if (tempFile->rename(saveFilePath)) {
        emit hashAndSaveFinished(true, QString(), tempFile, modelReply);
        ModelList::globalInstance()->updateModelsFromDirectory();
        return;
    }

    // Reopen the tempFile for copying
    if (!tempFile->open(QIODevice::ReadOnly)) {
        const QString error
            = QString("ERROR: Could not open temp file at finish: %1 %2").arg(tempFile->fileName()).arg(modelFilename);
        qWarning() << error;
        emit hashAndSaveFinished(false, error, tempFile, modelReply);
        return;
    }

    // Save the model file to disk
    QFile file(saveFilePath);
    if (file.open(QIODevice::WriteOnly)) {
        QByteArray buffer;
        while (!tempFile->atEnd()) {
            buffer = tempFile->read(16384);
            file.write(buffer);
        }
        file.close();
        tempFile->close();
        emit hashAndSaveFinished(true, QString(), tempFile, modelReply);
    } else {
        QFile::FileError error = file.error();
        const QString errorString
            = QString("ERROR: Could not save model to location: %1 failed with code %1").arg(saveFilePath).arg(error);
        qWarning() << errorString;
        tempFile->close();
        emit hashAndSaveFinished(false, errorString, tempFile, modelReply);
    }

    ModelList::globalInstance()->updateModelsFromDirectory();
}

void Download::handleModelDownloadFinished()
{
    QNetworkReply *modelReply = qobject_cast<QNetworkReply *>(sender());
    if (!modelReply)
        return;

    QString modelFilename = modelReply->request().attribute(QNetworkRequest::User).toString();
    QFile *tempFile = m_activeDownloads.value(modelReply);
    m_activeDownloads.remove(modelReply);

    if (modelReply->error()) {
        const QString errorString
            = QString("ERROR: Downloading failed with code %1 \"%2\"").arg(modelReply->error()).arg(modelReply->errorString());
        qWarning() << errorString;
        modelReply->deleteLater();
        tempFile->deleteLater();
        if (!hasRetry(modelFilename)) {
            QVector<QPair<int, QVariant>> data {
                { ModelList::DownloadingRole, false },
                { ModelList::DownloadErrorRole, errorString },
            };
            ModelList::globalInstance()->updateDataByFilename(modelFilename, data);
        }
        return;
    }

    clearRetry(modelFilename);

    // The hash and save needs the tempFile closed
    tempFile->close();

    if (!ModelList::globalInstance()->containsByFilename(modelFilename)) {
        qWarning() << "ERROR: downloading no such file:" << modelFilename;
        modelReply->deleteLater();
        tempFile->deleteLater();
        return;
    }

    // Notify that we are calculating hash
    ModelList::globalInstance()->updateDataByFilename(modelFilename, {{ ModelList::CalcHashRole, true }});
    QByteArray hash =  ModelList::globalInstance()->modelInfoByFilename(modelFilename).hash;
    ModelInfo::HashAlgorithm hashAlgorithm =  ModelList::globalInstance()->modelInfoByFilename(modelFilename).hashAlgorithm;
    const QString saveFilePath = MySettings::globalInstance()->modelPath() + modelFilename;
    emit requestHashAndSave(hash,
        (hashAlgorithm == ModelInfo::Md5 ? QCryptographicHash::Md5 : QCryptographicHash::Sha256),
        saveFilePath, tempFile, modelReply);
}

void Download::handleHashAndSaveFinished(bool success, const QString &error,
        QFile *tempFile, QNetworkReply *modelReply)
{
    // The hash and save should send back with tempfile closed
    Q_ASSERT(!tempFile->isOpen());
    QString modelFilename = modelReply->request().attribute(QNetworkRequest::User).toString();
    Network::globalInstance()->trackEvent("download_finished", { {"model", modelFilename}, {"success", success} });

    QVector<QPair<int, QVariant>> data {
        { ModelList::CalcHashRole, false },
        { ModelList::DownloadingRole, false },
    };

    modelReply->deleteLater();
    tempFile->deleteLater();

    if (!success) {
        data.append({ ModelList::DownloadErrorRole, error });
    } else {
        data.append({ ModelList::DownloadErrorRole, QString() });
        ModelInfo info = ModelList::globalInstance()->modelInfoByFilename(modelFilename);
        if (info.isDiscovered())
            ModelList::globalInstance()->updateDiscoveredInstalled(info);
    }

    ModelList::globalInstance()->updateDataByFilename(modelFilename, data);
}

void Download::handleReadyRead()
{
    QNetworkReply *modelReply = qobject_cast<QNetworkReply *>(sender());
    if (!modelReply)
        return;

    QFile *tempFile = m_activeDownloads.value(modelReply);
    QByteArray buffer;
    while (!modelReply->atEnd()) {
        buffer = modelReply->read(16384);
        tempFile->write(buffer);
    }
    tempFile->flush();
}
