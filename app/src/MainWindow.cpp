#include "MainWindow.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

const QStringList kExtensions = {"pm5s", "pwmb", "pmx2"}; // must match device/printapi.lua
const QString kDefaultHost = QStringLiteral("192.168.40.157");
constexpr int kMaxNameLength = 200;

QString humanSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString humanDuration(qint64 secs)
{
    const qint64 d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
    if (d > 0)
        return QStringLiteral("%1d %2h %3m").arg(d).arg(h).arg(m);
    if (h > 0)
        return QStringLiteral("%1h %2m").arg(h).arg(m);
    return QStringLiteral("%1m %2s").arg(m).arg(secs % 60);
}

// Table item that sorts by a numeric value in UserRole instead of its display text.
class NumericItem : public QTableWidgetItem
{
public:
    NumericItem(const QString &text, qint64 value)
        : QTableWidgetItem(text)
    {
        setData(Qt::UserRole, value);
    }
    bool operator<(const QTableWidgetItem &other) const override
    {
        return data(Qt::UserRole).toLongLong() < other.data(Qt::UserRole).toLongLong();
    }
};

void setPill(QLabel *label, const QString &text, const QString &color)
{
    label->setText(text);
    label->setStyleSheet(QStringLiteral(
        "QLabel { color: white; background: %1; border-radius: 4px; padding: 2px 8px; font-weight: bold; }")
                             .arg(color));
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("M5s Print Monitor"));
    setAcceptDrops(true);
    buildUi();

    QSettings s;
    m_hostEdit->setText(s.value("host", kDefaultHost).toString());
    m_intervalSpin->setValue(s.value("pollSeconds", 3).toInt());
    restoreGeometry(s.value("geometry").toByteArray());

    connect(&m_api, &ApiClient::statusReceived, this, &MainWindow::onStatus);
    connect(&m_api, &ApiClient::statusFailed, this, &MainWindow::onStatusFailed);
    connect(&m_api, &ApiClient::filesReceived, this, &MainWindow::onFiles);
    connect(&m_api, &ApiClient::filesFailed, this, [this](const QString &err) {
        // Keep showing the last listing; just report the failure.
        const bool wasRefresh = !m_refreshBtn->isEnabled();
        setRefreshIdle();
        if (wasRefresh)
            QMessageBox::warning(this, tr("Refresh failed"), tr("Could not read the printer's storage:\n%1").arg(err));
        else
            statusBar()->showMessage(tr("File list failed: %1").arg(err), 5000);
    });
    connect(&m_api, &ApiClient::uploadProgress, this, [this](qint64 sent, qint64 total) {
        if (total > 0) {
            m_uploadBar->setMaximum(1000);
            m_uploadBar->setValue(static_cast<int>(sent * 1000 / total));
            m_uploadBar->setFormat(QStringLiteral("%1 / %2").arg(humanSize(sent), humanSize(total)));
        }
    });
    connect(&m_api, &ApiClient::uploadFinished, this, &MainWindow::onUploadFinished);
    connect(&m_api, &ApiClient::uploadConflict, this, &MainWindow::onUploadConflict);
    connect(&m_api, &ApiClient::uploadFailed, this, &MainWindow::onUploadFailed);
    connect(&m_api, &ApiClient::deleteFinished, this, [this](const QStringList &deleted, const QStringList &notDeleted) {
        m_deleteBtn->setEnabled(!m_fileTable->selectionModel()->selectedRows().isEmpty());
        statusBar()->showMessage(tr("Deleted %n file(s)", nullptr, deleted.size()), 5000);
        if (!notDeleted.isEmpty())
            QMessageBox::warning(this, tr("Delete"), tr("Not deleted:\n%1").arg(notDeleted.join('\n')));
        m_api.fetchFiles();
    });
    connect(&m_api, &ApiClient::deleteFailed, this, [this](const QString &err) {
        m_deleteBtn->setEnabled(!m_fileTable->selectionModel()->selectedRows().isEmpty());
        statusBar()->clearMessage();
        QMessageBox::warning(this, tr("Delete failed"), tr("Could not delete:\n%1").arg(err));
    });

    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::pollStatus);
    connect(m_intervalSpin, &QSpinBox::valueChanged, this, [this](int secs) {
        m_pollTimer->setInterval(secs * 1000);
    });
    m_pollTimer->setInterval(m_intervalSpin->value() * 1000);

    applyHost();
    m_pollTimer->start();
}

MainWindow::~MainWindow()
{
    QSettings s;
    s.setValue("host", m_hostEdit->text().trimmed());
    s.setValue("pollSeconds", m_intervalSpin->value());
    s.setValue("geometry", saveGeometry());
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);

    // --- connection bar -------------------------------------------------
    auto *connRow = new QHBoxLayout;
    connRow->addWidget(new QLabel(tr("Printer:")));
    m_hostEdit = new QLineEdit;
    m_hostEdit->setPlaceholderText(tr("IP or hostname"));
    m_hostEdit->setMaximumWidth(220);
    connRow->addWidget(m_hostEdit);
    auto *connectBtn = new QPushButton(tr("Connect"));
    connRow->addWidget(connectBtn);
    connRow->addSpacing(16);
    connRow->addWidget(new QLabel(tr("Poll every")));
    m_intervalSpin = new QSpinBox;
    m_intervalSpin->setRange(1, 60);
    m_intervalSpin->setSuffix(tr(" s"));
    connRow->addWidget(m_intervalSpin);
    connRow->addStretch();
    m_connLabel = new QLabel;
    connRow->addWidget(m_connLabel);
    root->addLayout(connRow);

    connect(connectBtn, &QPushButton::clicked, this, &MainWindow::applyHost);
    connect(m_hostEdit, &QLineEdit::returnPressed, this, &MainWindow::applyHost);

    auto *splitter = new QSplitter(Qt::Horizontal);

    // --- status panel ---------------------------------------------------
    auto *statusBox = new QGroupBox(tr("Status"));
    auto *statusLayout = new QVBoxLayout(statusBox);
    auto *form = new QFormLayout;
    m_stateLabel = new QLabel(tr("—"));
    m_storageLabel = new QLabel(tr("—"));
    m_uptimeLabel = new QLabel(tr("—"));
    m_updatedLabel = new QLabel(tr("—"));
    form->addRow(tr("State:"), m_stateLabel);
    form->addRow(tr("Storage:"), m_storageLabel);
    form->addRow(tr("Uptime:"), m_uptimeLabel);
    form->addRow(tr("Last update:"), m_updatedLabel);
    statusLayout->addLayout(form);
    statusLayout->addWidget(new QLabel(tr("Job data (raw from printer):")));
    m_jobTree = new QTreeWidget;
    m_jobTree->setColumnCount(2);
    m_jobTree->setHeaderLabels({tr("Key"), tr("Value")});
    m_jobTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    statusLayout->addWidget(m_jobTree, 1);
    splitter->addWidget(statusBox);

    // --- files panel ----------------------------------------------------
    auto *filesBox = new QGroupBox(tr("Files on printer"));
    auto *filesLayout = new QVBoxLayout(filesBox);
    m_fileTable = new QTableWidget(0, 3);
    m_fileTable->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified")});
    m_fileTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_fileTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_fileTable->verticalHeader()->hide();
    m_fileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fileTable->setSortingEnabled(true);
    m_fileTable->sortByColumn(2, Qt::DescendingOrder); // newest first
    filesLayout->addWidget(m_fileTable, 1);

    m_spaceBar = new QProgressBar;
    m_spaceBar->setTextVisible(true);
    m_spaceBar->setFormat(tr("Storage: unknown"));
    m_spaceBar->setValue(0);
    filesLayout->addWidget(m_spaceBar);

    m_listingLabel = new QLabel(tr("No listing yet — press Refresh to read the printer's storage."));
    m_listingLabel->setWordWrap(true);
    filesLayout->addWidget(m_listingLabel);

    auto *btnRow = new QHBoxLayout;
    m_uploadBtn = new QPushButton(tr("Upload…"));
    m_deleteBtn = new QPushButton(tr("Delete"));
    m_refreshBtn = new QPushButton(tr("Refresh"));
    m_deleteBtn->setEnabled(false);
    m_uploadBtn->setEnabled(false); // enabled once the printer is reachable and idle
    m_refreshBtn->setToolTip(tr("Re-read the printer's Local storage. The printer's Local tab is "
                                "unavailable for ~10 s while the storage is borrowed."));
    btnRow->addWidget(m_uploadBtn);
    btnRow->addWidget(m_deleteBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_refreshBtn);
    filesLayout->addLayout(btnRow);

    auto *hint = new QLabel(tr("Tip: drop .pm5s files anywhere on this window to upload."));
    hint->setEnabled(false);
    filesLayout->addWidget(hint);
    splitter->addWidget(filesBox);

    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    root->addWidget(splitter, 1);

    connect(m_uploadBtn, &QPushButton::clicked, this, &MainWindow::chooseFilesToUpload);
    connect(m_deleteBtn, &QPushButton::clicked, this, &MainWindow::deleteSelected);
    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshFiles);
    connect(m_fileTable, &QTableWidget::itemSelectionChanged, this, [this] {
        m_deleteBtn->setEnabled(!m_fileTable->selectionModel()->selectedRows().isEmpty());
    });

    // --- upload progress row -------------------------------------------
    auto *upRow = new QHBoxLayout;
    m_uploadLabel = new QLabel;
    m_uploadBar = new QProgressBar;
    m_cancelBtn = new QPushButton(tr("Cancel"));
    upRow->addWidget(m_uploadLabel);
    upRow->addWidget(m_uploadBar, 1);
    upRow->addWidget(m_cancelBtn);
    root->addLayout(upRow);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this] {
        m_uploadQueue.clear();
        m_api.cancelUpload();
    });
    setUploadIdle();

    setCentralWidget(central);
    resize(1000, 600);
    setConnection(false, tr("not connected"));
}

// --------------------------------------------------------------------------
// Connection / polling
// --------------------------------------------------------------------------

void MainWindow::applyHost()
{
    m_api.setHost(m_hostEdit->text());
    m_haveStatus = false;
    m_filesUpdated = -1;
    m_cloudJob = false;
    setConnection(false, tr("connecting…"));
    pollStatus();
    m_api.fetchFiles();
}

void MainWindow::pollStatus()
{
    if (!m_api.host().isEmpty())
        m_api.fetchStatus();
}

void MainWindow::setConnection(bool online, const QString &detail)
{
    if (online)
        setPill(m_connLabel, tr("Online"), "#2e7d32");
    else
        setPill(m_connLabel, tr("Offline"), "#c62828");
    m_connLabel->setToolTip(detail);
    if (!online)
        statusBar()->showMessage(detail);
}

void MainWindow::onStatus(const QJsonObject &status)
{
    const bool wasOnline = m_haveStatus;
    m_haveStatus = true;
    setConnection(true, m_api.host());
    if (!wasOnline)
        statusBar()->clearMessage();

    m_cloudJob = status.value("cloud_job").toBool();
    if (m_cloudJob)
        setPill(m_stateLabel, tr("Receiving cloud job"), "#1565c0");
    else
        setPill(m_stateLabel, tr("Idle"), "#616161");

    const QJsonObject storage = status.value("storage").toObject();
    const bool linuxSide = storage.value("owner").toString() == QLatin1String("linux");
    if (!linuxSide)
        m_storageLabel->setText(tr("With printer"));
    else if (storage.value("busy").toBool())
        m_storageLabel->setText(tr("Borrowed for a transfer"));
    else
        m_storageLabel->setText(tr("In use by cloud service"));

    // The server bumps files_updated whenever an upload/delete/refresh re-reads the storage.
    const QJsonValue fu = status.value("files_updated");
    const qint64 filesUpdated = fu.isDouble() ? static_cast<qint64>(fu.toDouble()) : 0;
    if (filesUpdated != m_filesUpdated || !wasOnline)
        m_api.fetchFiles(); // cached listing; never touches the printer's storage
    m_uploadBtn->setEnabled(!m_cloudJob);

    const QJsonValue up = status.value("uptime");
    m_uptimeLabel->setText(up.isDouble() ? humanDuration(static_cast<qint64>(up.toDouble())) : tr("—"));
    m_updatedLabel->setText(QTime::currentTime().toString("HH:mm:ss"));

    showJsonTree(status);
}

void MainWindow::onStatusFailed(const QString &error)
{
    m_haveStatus = false;
    setConnection(false, tr("Cannot reach %1: %2").arg(m_api.host(), error));
    m_stateLabel->setText(tr("—"));
    m_stateLabel->setStyleSheet({});
    m_uploadBtn->setEnabled(false);
}

void MainWindow::showJsonTree(const QJsonObject &status)
{
    // Remember which top-level sections the user collapsed so polling doesn't fight them.
    QSet<QString> collapsed;
    for (int i = 0; i < m_jobTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = m_jobTree->topLevelItem(i);
        if (!it->isExpanded())
            collapsed.insert(it->text(0));
    }

    m_jobTree->clear();
    bool any = false;
    for (const char *key : {"print", "local_print", "msg"}) {
        const QJsonValue v = status.value(QLatin1String(key));
        if (v.isNull() || v.isUndefined())
            continue;
        any = true;
        auto *item = new QTreeWidgetItem(m_jobTree, {QLatin1String(key)});
        if (v.isObject() || v.isArray()) {
            const QJsonObject obj = v.isObject() ? v.toObject() : QJsonObject{{"items", v}};
            for (auto it = obj.begin(); it != obj.end(); ++it)
                addJsonValue(item, it.key(), it.value());
        } else {
            item->setText(1, v.toVariant().toString());
        }
        item->setExpanded(!collapsed.contains(item->text(0)));
    }
    if (!any)
        new QTreeWidgetItem(m_jobTree, {tr("(no active job)")});
}

void MainWindow::addJsonValue(QTreeWidgetItem *parent, const QString &key, const QJsonValue &value)
{
    auto *item = new QTreeWidgetItem(parent, {key});
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it)
            addJsonValue(item, it.key(), it.value());
        item->setExpanded(true);
    } else if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (int i = 0; i < arr.size(); ++i)
            addJsonValue(item, QStringLiteral("[%1]").arg(i), arr.at(i));
        item->setExpanded(true);
    } else if (value.isNull()) {
        item->setText(1, QStringLiteral("null"));
    } else {
        item->setText(1, value.toVariant().toString());
    }
}

void MainWindow::refreshFiles()
{
    if (m_api.refreshInProgress())
        return;
    m_refreshBtn->setEnabled(false);
    m_refreshBtn->setText(tr("Reading…"));
    statusBar()->showMessage(tr("Borrowing the printer's storage to read the file list "
                                "(its Local tab is unavailable for ~10 s)…"));
    m_api.fetchFiles(true);
}

void MainWindow::setRefreshIdle()
{
    m_refreshBtn->setEnabled(true);
    m_refreshBtn->setText(tr("Refresh"));
}

void MainWindow::onFiles(const QList<ApiClient::FileEntry> &files, qint64 freeBytes, qint64 totalBytes,
                         const QDateTime &updated, bool refreshed)
{
    if (refreshed) {
        setRefreshIdle();
        statusBar()->showMessage(tr("File list re-read from the printer"), 5000);
    }
    m_filesUpdated = updated.isValid() ? updated.toSecsSinceEpoch() : 0;
    m_listingLabel->setText(updated.isValid()
                                ? tr("Listing as of %1 — updated after each upload/delete; "
                                     "press Refresh to re-read.")
                                      .arg(QLocale().toString(updated, QLocale::ShortFormat))
                                : tr("No listing yet — press Refresh to read the printer's storage."));

    QSet<QString> selected;
    for (const QModelIndex &idx : m_fileTable->selectionModel()->selectedRows())
        selected.insert(m_fileTable->item(idx.row(), 0)->text());

    m_fileTable->setSortingEnabled(false);
    m_fileTable->setRowCount(0);
    for (const auto &f : files) {
        const int row = m_fileTable->rowCount();
        m_fileTable->insertRow(row);
        m_fileTable->setItem(row, 0, new QTableWidgetItem(f.name));
        auto *size = new NumericItem(humanSize(f.size), f.size);
        size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_fileTable->setItem(row, 1, size);
        m_fileTable->setItem(row, 2, new NumericItem(QLocale().toString(f.mtime, QLocale::ShortFormat),
                                                     f.mtime.toSecsSinceEpoch()));
        if (selected.contains(f.name))
            m_fileTable->selectRow(row);
    }
    m_fileTable->setSortingEnabled(true);

    if (totalBytes > 0 && freeBytes >= 0) {
        const qint64 used = totalBytes - freeBytes;
        m_spaceBar->setMaximum(1000);
        m_spaceBar->setValue(static_cast<int>(used * 1000 / totalBytes));
        m_spaceBar->setFormat(tr("%1 free of %2").arg(humanSize(freeBytes), humanSize(totalBytes)));
    } else {
        m_spaceBar->setValue(0);
        m_spaceBar->setFormat(tr("Storage: unknown"));
    }
}

// --------------------------------------------------------------------------
// Uploads
// --------------------------------------------------------------------------

QString MainWindow::remoteNameFor(const QString &localPath, QString *error)
{
    const QFileInfo fi(localPath);
    if (!kExtensions.contains(fi.suffix().toLower())) {
        *error = tr("unsupported file type (expected %1)").arg("." + kExtensions.join(", ."));
        return {};
    }
    // Mirror the server's allowed character set: letters, digits, space and . _ - ( ) [ ] + ,
    QString name = fi.fileName();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._\\- ()\\[\\]+,]")), QStringLiteral("_"));
    while (name.startsWith('.'))
        name.remove(0, 1);
    if (name.size() > kMaxNameLength) {
        const QString ext = "." + fi.suffix();
        name = name.left(kMaxNameLength - ext.size()) + ext;
    }
    return name;
}

void MainWindow::chooseFilesToUpload()
{
    QSettings s;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Upload print files"), s.value("lastDir").toString(),
        tr("Print files (%1);;All files (*)").arg("*." + kExtensions.join(" *.")));
    if (paths.isEmpty())
        return;
    s.setValue("lastDir", QFileInfo(paths.first()).absolutePath());
    enqueueUploads(paths);
}

void MainWindow::enqueueUploads(const QStringList &paths)
{
    QStringList rejected;
    for (const QString &p : paths) {
        QString err;
        if (remoteNameFor(p, &err).isEmpty())
            rejected << QStringLiteral("%1: %2").arg(QFileInfo(p).fileName(), err);
        else
            m_uploadQueue << p;
    }
    if (!rejected.isEmpty())
        QMessageBox::warning(this, tr("Skipped files"), rejected.join('\n'));
    startNextUpload();
}

void MainWindow::startNextUpload()
{
    if (m_api.uploadInProgress())
        return;
    if (m_uploadQueue.isEmpty()) {
        setUploadIdle();
        return;
    }
    m_currentUploadPath = m_uploadQueue.takeFirst();
    QString err;
    const QString name = remoteNameFor(m_currentUploadPath, &err);

    m_uploadLabel->setText(m_uploadQueue.isEmpty()
                               ? tr("Uploading %1").arg(name)
                               : tr("Uploading %1 (%2 more queued)").arg(name).arg(m_uploadQueue.size()));
    m_uploadLabel->show();
    m_uploadBar->setRange(0, 1000);
    m_uploadBar->setValue(0);
    m_uploadBar->setFormat(tr("borrowing printer storage…"));
    m_uploadBar->show();
    m_cancelBtn->show();
    m_api.upload(m_currentUploadPath, name, false);
}

void MainWindow::onUploadFinished(const QString &name, qint64 bytes)
{
    statusBar()->showMessage(tr("Uploaded %1 (%2)").arg(name, humanSize(bytes)), 8000);
    m_api.fetchFiles();
    startNextUpload();
}

void MainWindow::onUploadConflict(const QString &localPath, const QString &remoteName)
{
    const auto choice = QMessageBox::question(
        this, tr("File exists"),
        tr("%1 already exists on the printer. Replace it?").arg(remoteName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice == QMessageBox::Yes) {
        m_uploadBar->setValue(0);
        m_api.upload(localPath, remoteName, true);
    } else {
        startNextUpload();
    }
}

void MainWindow::onUploadFailed(const QString &name, const QString &error)
{
    if (error == QLatin1String("cancelled")) {
        statusBar()->showMessage(tr("Upload of %1 cancelled").arg(name), 5000);
        m_uploadQueue.clear();
        setUploadIdle();
        return;
    }
    QMessageBox::warning(this, tr("Upload failed"), tr("Could not upload %1:\n%2").arg(name, error));
    startNextUpload();
}

void MainWindow::setUploadIdle()
{
    m_currentUploadPath.clear();
    m_uploadLabel->hide();
    m_uploadBar->hide();
    m_cancelBtn->hide();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    for (const QUrl &u : event->mimeData()->urls()) {
        if (u.isLocalFile() && kExtensions.contains(QFileInfo(u.toLocalFile()).suffix().toLower())) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &u : event->mimeData()->urls())
        if (u.isLocalFile())
            paths << u.toLocalFile();
    if (paths.isEmpty())
        return;
    if (!m_haveStatus || m_cloudJob) {
        QMessageBox::warning(this, tr("Cannot upload"),
                             m_haveStatus ? tr("The printer is busy receiving a cloud job.")
                                          : tr("The printer is not reachable."));
        return;
    }
    event->acceptProposedAction();
    enqueueUploads(paths);
}

// --------------------------------------------------------------------------
// Delete
// --------------------------------------------------------------------------

void MainWindow::deleteSelected()
{
    QStringList names;
    for (const QModelIndex &idx : m_fileTable->selectionModel()->selectedRows())
        names << m_fileTable->item(idx.row(), 0)->text();
    if (names.isEmpty())
        return;

    const auto choice = QMessageBox::question(
        this, tr("Delete files"),
        tr("Delete %n file(s) from the printer?\n\n%1", nullptr, names.size()).arg(names.join('\n')),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes)
        return;
    m_deleteBtn->setEnabled(false);
    statusBar()->showMessage(tr("Deleting… (the printer's Local tab is briefly unavailable)"));
    m_api.deleteFiles(names);
}
