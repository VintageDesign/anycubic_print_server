#pragma once

#include "ApiClient.h"

#include <QJsonObject>
#include <QMainWindow>
#include <QStringList>

class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void buildUi();
    void applyHost();
    void pollStatus();

    void onStatus(const QJsonObject &status);
    void onStatusFailed(const QString &error);
    void onFiles(const QList<ApiClient::FileEntry> &files, qint64 freeBytes, qint64 totalBytes,
                 const QDateTime &updated, bool refreshed);
    void refreshFiles();
    void setRefreshIdle();

    void chooseFilesToUpload();
    void enqueueUploads(const QStringList &paths);
    void startNextUpload();
    void onUploadFinished(const QString &name, qint64 bytes);
    void onUploadConflict(const QString &localPath, const QString &remoteName);
    void onUploadFailed(const QString &name, const QString &error);
    void setUploadIdle();

    void deleteSelected();

    void setConnection(bool online, const QString &detail);
    void showJsonTree(const QJsonObject &status);
    static void addJsonValue(QTreeWidgetItem *parent, const QString &key, const QJsonValue &value);
    static QString remoteNameFor(const QString &localPath, QString *error);

    ApiClient m_api;
    QTimer *m_pollTimer = nullptr;
    QStringList m_uploadQueue;
    QString m_currentUploadPath;
    qint64 m_filesUpdated = -1; // server cache timestamp we last displayed
    bool m_cloudJob = false;
    bool m_haveStatus = false;

    // Connection bar
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QLabel *m_connLabel = nullptr;

    // Status
    QLabel *m_stateLabel = nullptr;
    QLabel *m_storageLabel = nullptr;
    QLabel *m_uptimeLabel = nullptr;
    QLabel *m_updatedLabel = nullptr;
    QTreeWidget *m_jobTree = nullptr;

    // Files
    QTableWidget *m_fileTable = nullptr;
    QProgressBar *m_spaceBar = nullptr;
    QLabel *m_listingLabel = nullptr;
    QPushButton *m_uploadBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;

    // Upload progress
    QLabel *m_uploadLabel = nullptr;
    QProgressBar *m_uploadBar = nullptr;
    QPushButton *m_cancelBtn = nullptr;
};
