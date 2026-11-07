#include "ApiClient.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {
constexpr int kQueryTimeoutMs = 5000;        // status / cached file list
constexpr int kStorageTimeoutMs = 90000;     // requests that borrow the eMMC (refresh, delete)
constexpr int kUploadStallTimeoutMs = 60000; // no progress for this long aborts an upload;
                                             // the server spends ~8 s switching storage first
} // namespace

ApiClient::ApiClient(QObject *parent)
    : QObject(parent)
{
}

void ApiClient::setHost(const QString &host)
{
    QString h = host.trimmed();
    if (h.startsWith("http://", Qt::CaseInsensitive))
        h = h.mid(7);
    while (h.endsWith('/'))
        h.chop(1);
    m_host = h;
}

QUrl ApiClient::endpoint(const QString &name, const QUrlQuery &query) const
{
    QUrl url(QStringLiteral("http://%1/cgi-bin/api/%2").arg(m_host, name));
    if (!query.isEmpty())
        url.setQuery(query);
    return url;
}

bool ApiClient::parseReply(QNetworkReply *reply, QJsonObject &obj, QString &error)
{
    const QByteArray body = reply->readAll();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error == QJsonParseError::NoError && doc.isObject()) {
        obj = doc.object();
        if (obj.value("ok").toBool())
            return true;
        error = obj.value("error").toString(QStringLiteral("request failed"));
        if (http)
            error += QStringLiteral(" (HTTP %1)").arg(http);
        return false;
    }

    if (reply->error() == QNetworkReply::OperationCanceledError)
        error = QStringLiteral("timed out"); // transfer timeouts abort the reply
    else if (reply->error() != QNetworkReply::NoError)
        error = reply->errorString();
    else if (http)
        error = QStringLiteral("unexpected response (HTTP %1)").arg(http);
    else
        error = QStringLiteral("unexpected response");
    return false;
}

void ApiClient::fetchStatus()
{
    if (m_statusReply)
        return; // previous poll still running
    QNetworkRequest req(endpoint("status"));
    req.setTransferTimeout(kQueryTimeoutMs);
    m_statusReply = m_nam.get(req);
    QNetworkReply *reply = m_statusReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_statusReply = nullptr;
        QJsonObject obj;
        QString err;
        if (parseReply(reply, obj, err))
            emit statusReceived(obj);
        else
            emit statusFailed(err);
    });
}

void ApiClient::fetchFiles(bool refresh)
{
    if (m_filesReply) {
        if (!refresh || m_filesRefreshing)
            return; // an equal or better request is already running
        // Upgrade an in-flight cached fetch to a refresh.
        m_filesReply->disconnect(this);
        m_filesReply->abort();
        m_filesReply->deleteLater();
        m_filesReply = nullptr;
    }
    QUrlQuery q;
    if (refresh)
        q.addQueryItem("refresh", "1");
    QNetworkRequest req(endpoint("files", q));
    req.setTransferTimeout(refresh ? kStorageTimeoutMs : kQueryTimeoutMs);
    m_filesRefreshing = refresh;
    m_filesReply = m_nam.get(req);
    QNetworkReply *reply = m_filesReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_filesReply = nullptr;
        m_filesRefreshing = false;
        QJsonObject obj;
        QString err;
        if (!parseReply(reply, obj, err)) {
            emit filesFailed(err);
            return;
        }
        QList<FileEntry> files;
        for (const QJsonValue &v : obj.value("files").toArray()) {
            const QJsonObject f = v.toObject();
            FileEntry e;
            e.name = f.value("name").toString();
            e.size = static_cast<qint64>(f.value("size").toDouble());
            e.mtime = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(f.value("mtime").toDouble()));
            files.append(e);
        }
        const QJsonValue updated = obj.value("updated");
        emit filesReceived(files,
                           static_cast<qint64>(obj.value("free").toDouble(-1)),
                           static_cast<qint64>(obj.value("total").toDouble(-1)),
                           updated.isDouble()
                               ? QDateTime::fromSecsSinceEpoch(static_cast<qint64>(updated.toDouble()))
                               : QDateTime(),
                           obj.value("refreshed").toBool());
    });
}

void ApiClient::upload(const QString &localPath, const QString &remoteName, bool overwrite)
{
    if (m_uploadReply) {
        emit uploadFailed(remoteName, QStringLiteral("another upload is in progress"));
        return;
    }

    auto *file = new QFile(localPath);
    if (!file->open(QIODevice::ReadOnly)) {
        const QString err = file->errorString();
        delete file;
        emit uploadFailed(remoteName, err);
        return;
    }

    QUrlQuery q;
    q.addQueryItem("name", remoteName);
    if (overwrite)
        q.addQueryItem("overwrite", "1");

    QNetworkRequest req(endpoint("upload", q));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setHeader(QNetworkRequest::ContentLengthHeader, file->size());
    req.setTransferTimeout(kUploadStallTimeoutMs);

    m_uploadCancelled = false;
    m_uploadReply = m_nam.post(req, file);
    QNetworkReply *reply = m_uploadReply;
    file->setParent(reply); // keep the file alive exactly as long as the reply

    connect(reply, &QNetworkReply::uploadProgress, this, &ApiClient::uploadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, localPath, remoteName] {
        reply->deleteLater();
        m_uploadReply = nullptr; // allow the next queued upload to start
        if (m_uploadCancelled) {
            emit uploadFailed(remoteName, QStringLiteral("cancelled"));
            return;
        }
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject obj;
        QString err;
        if (parseReply(reply, obj, err))
            emit uploadFinished(remoteName, static_cast<qint64>(obj.value("bytes").toDouble()));
        else if (http == 409)
            emit uploadConflict(localPath, remoteName);
        else
            emit uploadFailed(remoteName, err);
    });
}

void ApiClient::cancelUpload()
{
    if (m_uploadReply) {
        m_uploadCancelled = true;
        m_uploadReply->abort();
    }
}

void ApiClient::deleteFiles(const QStringList &names)
{
    QUrlQuery q;
    for (const QString &n : names)
        q.addQueryItem("name", n);
    QNetworkRequest req(endpoint("delete", q));
    req.setTransferTimeout(kStorageTimeoutMs);
    QNetworkReply *reply = m_nam.post(req, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        QJsonObject obj;
        QString err;
        const bool ok = parseReply(reply, obj, err);
        auto list = [&obj](const char *key) {
            QStringList out;
            for (const QJsonValue &v : obj.value(QLatin1String(key)).toArray())
                out << v.toString();
            return out;
        };
        // A 404 (nothing deleted) still carries the lists; only treat transport/other errors as failure.
        if (ok || obj.contains("deleted"))
            emit deleteFinished(list("deleted"), list("missing") + list("failed"));
        else
            emit deleteFailed(err);
    });
}
