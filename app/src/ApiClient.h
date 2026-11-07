#pragma once

// Thin async client for the printer's CGI API (see docs/http_api.md).

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrlQuery>

class QNetworkReply;

class ApiClient : public QObject
{
    Q_OBJECT

public:
    struct FileEntry
    {
        QString name;
        qint64 size = 0;
        QDateTime mtime;
    };

    explicit ApiClient(QObject *parent = nullptr);

    // Accepts "host", "host:port" or "http://host[:port]".
    void setHost(const QString &host);
    QString host() const { return m_host; }

    void fetchStatus();
    void fetchFiles(bool refresh = false); // refresh=true borrows the eMMC (~10 s)
    void upload(const QString &localPath, const QString &remoteName, bool overwrite);
    void cancelUpload();
    void deleteFiles(const QStringList &names);

    bool statusInFlight() const { return !m_statusReply.isNull(); }
    bool uploadInProgress() const { return !m_uploadReply.isNull(); }
    bool refreshInProgress() const { return !m_filesReply.isNull() && m_filesRefreshing; }

signals:
    void statusReceived(const QJsonObject &status);
    void statusFailed(const QString &error);

    void filesReceived(const QList<ApiClient::FileEntry> &files, qint64 freeBytes, qint64 totalBytes,
                       const QDateTime &updated, bool refreshed);
    void filesFailed(const QString &error);

    void uploadProgress(qint64 sent, qint64 total);
    void uploadFinished(const QString &remoteName, qint64 bytes);
    void uploadConflict(const QString &localPath, const QString &remoteName);
    void uploadFailed(const QString &remoteName, const QString &error);

    void deleteFinished(const QStringList &deleted, const QStringList &notDeleted);
    void deleteFailed(const QString &error);

private:
    QUrl endpoint(const QString &name, const QUrlQuery &query = {}) const;
    static bool parseReply(QNetworkReply *reply, QJsonObject &obj, QString &error);

    QNetworkAccessManager m_nam;
    QString m_host;
    QPointer<QNetworkReply> m_statusReply;
    QPointer<QNetworkReply> m_filesReply;
    QPointer<QNetworkReply> m_uploadReply;
    bool m_uploadCancelled = false;
    bool m_filesRefreshing = false;
};
